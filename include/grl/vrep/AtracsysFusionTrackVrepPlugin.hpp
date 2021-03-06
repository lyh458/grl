#ifndef _ATRACSYS_FUSION_TRACK_VREP_PLUGIN_HPP_
#define _ATRACSYS_FUSION_TRACK_VREP_PLUGIN_HPP_

#include <iostream>
#include <memory>
#include <exception>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <atomic>
#include <ctime>

#include <spdlog/spdlog.h>

#include <boost/exception/all.hpp>
#include <boost/lexical_cast.hpp>

#include "grl/vrep/Eigen.hpp"
#include "grl/vrep/Vrep.hpp"
#include "grl/time.hpp"

#include "grl/sensor/FusionTrack.hpp"
#include "grl/sensor/FusionTrackToEigen.hpp"
#include "grl/sensor/FusionTrackToFlatbuffer.hpp"

#include "v_repLib.h"

#include "grl/vector_ostream.hpp"

#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/util.h"
#include "flatbuffers/idl.h"

namespace grl
{

/// Creates a complete vrep plugin object
/// usage:
/// @code
///    auto kukaPluginPG = std::make_shared<grl::KukaVrepPlugin>();
///    kukaPluginPG->construct();
///    while(true) kukaPluginPG->run_one();
/// @endcode
///
/// @todo this implementation is a bit hacky, redesign it
/// @todo separate out grl specific code from general atracsys control code
/// @todo Template on robot driver and create a driver that just reads/writes to/from the simulation, then pass the two templates so the simulation and the real driver can be selected.
///
/// Conceptually, this class loads up the vrep string identifiers for the objects whose position you want to modify
/// using the AtracsysFusionTrack data. This consists of the Object whose position you wish to modify, the object whose frame
/// it should be modified in, and a bool stating if the optical tracker's measurement should be inverted before applying the
/// position. This class will then constantly collect measurments, then set the object positions from the frames specified
/// for every object detected.
///
/// @note Skips geometries found based on the ini file that aren't actively configured silently.
class AtracsysFusionTrackVrepPlugin : public std::enable_shared_from_this<AtracsysFusionTrackVrepPlugin>
{
public:
  /// @see MotionConfigParams
  /// @see VrepMotionConfigTuple
  enum MotionConfigParamsIndex
  {
    ObjectToMove,
    FrameInWhichToMoveObject,
    ObjectBeingMeasured,
    GeometryID ///< @note GeometryID does not apply to VrepMotionConfigTuple
  };

  typedef std::tuple<
      std::string, // ObjectToMove
      std::string, // FrameInWhichToMoveObject
      std::string, // ObjectBeingMeasured
      std::string  // GeometryID
      > MotionConfigParams;

  struct Params
  {
    /// the params for the underlying fusiontrack device
    grl::sensor::FusionTrack::Params FusionTrackParams;
    /// optical tracker base (frame of transform measurement)
    std::string OpticalTrackerBase;
    /// params for the objects, frames, and transform inversion to update object positions
    std::vector<MotionConfigParams> MotionConfigParamsVector;
  };

  static const Params defaultParams()
  {
    return moveBoneParams();
  }

  static const Params emptyDefaultParams()
  {
    std::vector<MotionConfigParams> motionConfigParams;

    return {grl::sensor::FusionTrack::emptyDefaultParams(),
            "OpticalTrackerBase#0",
            motionConfigParams};
  };

  static const Params moveTrackerParams()
  {
    std::vector<MotionConfigParams> motionConfigParams;
    motionConfigParams.push_back(std::make_tuple("Fiducial#22", "OpticalTrackerBase#0", "Fiducial#22", "22"));

    return {grl::sensor::FusionTrack::defaultParams(),
            "OpticalTrackerBase#0",
            motionConfigParams};
  }

  static const Params moveBoneParams()
  {
    std::vector<MotionConfigParams> motionConfigParams;
    motionConfigParams.push_back(std::make_tuple("OpticalTrackerBase#0", "Fiducial#55", "Fiducial#55", "55"));

    return {grl::sensor::FusionTrack::defaultParams(),
            "OpticalTrackerBase#0",
            motionConfigParams};
  }

  /// @todo allow parameters to be updated
  AtracsysFusionTrackVrepPlugin(Params params = defaultParams())
      : params_(params),
        isConnectionEstablished_(false),
        m_shouldStop(false)
  {
    /// @todo figure out how to re-enable when .so isn't loaded
    // initHandles();
  }

  /// construct() function completes initialization of the plugin
  /// @todo move this into the actual constructor, but need to correctly handle or attach vrep shared libraries for unit tests.
  void construct()
  {
    initHandles();
    m_driverThread.reset(new std::thread(&AtracsysFusionTrackVrepPlugin::update, this));
  }

  void destruct()
  {
    m_shouldStop = true;
    if (m_driverThread)
    {
      m_driverThread->join();
    }

    for(auto &saveThreadP : m_saveRecordingThreads)
    {
      m_driverThread->join();
    }
  }

  /// adds an object to active tracking, replacing existing objects with the same GeometryID
  void add_object(const MotionConfigParams mcp)
  {
    std::lock_guard<std::mutex> lock(m_frameAccess);
    MotionConfigParamsAddConfig(mcp, m_geometryIDToVrepMotionConfigMap);
  }

  /// @brief clears all actively tracked objects
  /// Does not modify fusiontrack params, such as any loaded geometry ini config files.
  void clear_objects()
  {
    std::lock_guard<std::mutex> lock(m_frameAccess);
    m_geometryIDToVrepMotionConfigMap.clear();
  }

  /// @brief Remove a geometry and the corresponding objects so they no longer receive tracking updates.
  /// Does not modify fusiontrack params, such as any loaded geometry ini config files.
  void remove_geometry(int GeometryID)
  {
    std::lock_guard<std::mutex> lock(m_frameAccess);
    m_geometryIDToVrepMotionConfigMap.erase(GeometryID);
  }

  /// @brief Remove a geometry and the corresponding objects so they no longer receive tracking updates.
  /// Does not modify fusiontrack params, such as any loaded geometry ini config files.
  void remove_geometry(std::string GeometryID)
  {
    remove_geometry(boost::lexical_cast<int>(GeometryID));
  }

  /// Is everything ok?
  /// @return true if the optical tracker is actively running without any issues
  /// @todo consider expanding to support real error codes
  bool is_active()
  {
    return allHandlesSet && !exceptionPtr && opticalTrackerP && isConnectionEstablished_;
  }

  /// Is the optical tracker plugin currently recording log data?
  bool is_recording()
  {
    return is_active() && m_isRecording;
  }

  void run_one()
  {

    // rethrow an exception if it occured in the other thread.
    if (exceptionPtr)
    {
      /// note: this exception most likely came from the update() call initializing opticalTrackerP
      std::rethrow_exception(exceptionPtr);
    }

    // don't try to lock or start sending the tracker data
    // until the device has established a connection
    if (!isConnectionEstablished_ || !allHandlesSet) {
      return;
    }


    std::lock_guard<std::mutex> lock(m_frameAccess);

    // if any of the components haven't finished initializing, halt the program with an error
    BOOST_VERIFY(m_receivedFrame && m_nextState && opticalTrackerP);

    Eigen::Affine3f cameraToMarkerTransform; /// Relative distance between camera and marker?

    for(auto &marker : m_receivedFrame->Markers)
    {

      cameraToMarkerTransform = sensor::ftkMarkerToAffine3f(marker);
      auto configIterator = m_geometryIDToVrepMotionConfigMap.find(marker.geometryId);
      if (configIterator == m_geometryIDToVrepMotionConfigMap.end()) continue; // no configuration for this item
      auto config = configIterator->second;

      // invert the transform from the tracker to the object if needed
      if (m_opticalTrackerBase == std::get<ObjectToMove>(config) &&
          std::get<FrameInWhichToMoveObject>(config) == std::get<ObjectBeingMeasured>(config))
      {
        cameraToMarkerTransform = cameraToMarkerTransform.inverse();
      }
      else if (std::get<FrameInWhichToMoveObject>(config) != m_opticalTrackerBase)
      {
        BOOST_THROW_EXCEPTION(std::runtime_error("AtracsysFusionTrackVrepPlugin: moving objects other than those being measured and the base itself are not yet supported."));
      }

      setObjectTransform(std::get<ObjectToMove>(config), std::get<FrameInWhichToMoveObject>(config), cameraToMarkerTransform);
    }
  }

  ~AtracsysFusionTrackVrepPlugin()
  {
    destruct();
  }

  /// start recording the fusiontrack frame data in memory
  /// return true on success, false on failure
  bool start_recording()
  {

    m_isRecording = true;
    return m_isRecording;
  }
  /// stop recording the fusiontrack frame data in memory
  /// return true on success, false on failure
  bool stop_recording()
  {
    m_isRecording = false;
    return !m_isRecording;
  }

  /// save the currently recorded fusiontrack frame data, this also clears the recording
  bool save_recording(std::string filename)
  {
    if(filename.empty())
    {
      filename = current_date_and_time_string() + "FusionTrack.flik";
    }
    std::cout <<"Save Recording..." << filename << std::endl;
    /// Uncomment the line below to call the save_recording function in update()
    /// lock mutex before accessing file

    std::lock_guard<std::mutex> lock(m_frameAccess);

    // std::move std::move is used to indicate that an object (m_logFileBufferBuilderP) may be "moved from",
    // i.e. allowing the efficient transfer of resources from m_logFileBufferBuilderP to another objec.
    // then save_fbbP = m_logFileBufferBuilderP, and m_logFileBufferBuilderP is nullptr
    // Lambda function: [capture](parameters)->return-type {body}
    // [ = ]: captures all variables used in the lambda by value
    // Lambda functions are just syntactic sugar for inline and anonymous functors.
    // https://stackoverflow.com/questions/7627098/what-is-a-lambda-expression-in-c11

    auto saveLambdaFunction = [
      save_fbbP = std::move(m_logFileBufferBuilderP),
      save_KUKAiiwaFusionTrackMessageBufferP = std::move(m_KUKAiiwaFusionTrackMessageBufferP),
      filename
    ]() mutable
    {
      flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<grl::flatbuffer::KUKAiiwaFusionTrackMessage>>> states = save_fbbP->CreateVector(*save_KUKAiiwaFusionTrackMessageBufferP);
      flatbuffers::Offset<grl::flatbuffer::LogKUKAiiwaFusionTrack> fbLogKUKAiiwaFusionTrack = grl::flatbuffer::CreateLogKUKAiiwaFusionTrack(*save_fbbP, states);
      save_fbbP->Finish(fbLogKUKAiiwaFusionTrack, grl::flatbuffer::LogKUKAiiwaFusionTrackIdentifier());
      auto verifier = flatbuffers::Verifier(save_fbbP->GetBufferPointer(), save_fbbP->GetSize());
      bool success = grl::flatbuffer::VerifyLogKUKAiiwaFusionTrackBuffer(verifier);
      std::cout << "filename: " << filename << " verifier success: " << success << std::endl;
      // Write data to file
      flatbuffers::SaveFile(filename.c_str(), reinterpret_cast<const char *>(save_fbbP->GetBufferPointer()), save_fbbP->GetSize(), true);
    };
    // save the recording to a file in a separate thread, memory will be freed up when file finishes saving
    std::shared_ptr<std::thread> saveLogThread(std::make_shared<std::thread>(saveLambdaFunction));
    m_saveRecordingThreads.push_back(saveLogThread);
    // flatbuffersbuilder does not yet exist
    m_logFileBufferBuilderP = std::make_shared<flatbuffers::FlatBufferBuilder>();
    m_KUKAiiwaFusionTrackMessageBufferP =
        std::make_shared<std::vector<flatbuffers::Offset<grl::flatbuffer::KUKAiiwaFusionTrackMessage>>>();
  }

  // clear the recording buffer from memory immediately to start fresh
  void clear_recording()
  {
    std::lock_guard<std::mutex> lock(m_frameAccess);
    m_logFileBufferBuilderP.reset();
    m_KUKAiiwaFusionTrackMessageBufferP.reset();
  }


private:
  /// @todo support boost::asio
  /// Reads data off of the real optical tracker device in a separate thread
  void update()
  {
    try
    {
      // initialize all of the real device states
      std::lock_guard<std::mutex> lock(m_frameAccess);
      opticalTrackerP.reset(new grl::sensor::FusionTrack(params_.FusionTrackParams));
      // std::move is used to indicate that an object t may be "moved from",
      // i.e. allowing the efficient transfer of resources from t to another object.
      m_receivedFrame = std::move(opticalTrackerP->makeFramePtr());
      m_nextState = std::move(opticalTrackerP->makeFramePtr());
      isConnectionEstablished_ = true;
    }
    catch (...)
    {
      // transport the exception to the main thread in a safe manner
      exceptionPtr = std::current_exception();
      m_shouldStop = true;
    }

    // run the primary update loop in a separate thread
    int counter = 0;
    while (!m_shouldStop)
    {
      opticalTrackerP->receive(*m_nextState);
      {
          std::lock_guard<std::mutex> lock(m_frameAccess);
          if (m_isRecording)
          {
            // convert the buffer into a flatbuffer for recording and add it to the in memory buffer
            // @todo TODO(ahundt) if there haven't been problems, delete this todo, but if recording in the driver thread is time consuming move the code to another thread
            if (!m_logFileBufferBuilderP)
            {
              // flatbuffersbuilder does not yet exist
              m_logFileBufferBuilderP = std::make_shared<flatbuffers::FlatBufferBuilder>();
              m_KUKAiiwaFusionTrackMessageBufferP =
                  std::make_shared<std::vector<flatbuffers::Offset<grl::flatbuffer::KUKAiiwaFusionTrackMessage>>>();
            }
            BOOST_VERIFY(m_logFileBufferBuilderP != nullptr);
            BOOST_VERIFY(opticalTrackerP != nullptr);
            BOOST_VERIFY(m_nextState != nullptr);
            flatbuffers::Offset<grl::flatbuffer::KUKAiiwaFusionTrackMessage> oneKUKAiiwaFusionTrackMessage =
                grl::toFlatBuffer(*m_logFileBufferBuilderP, *opticalTrackerP, *m_nextState);
            m_KUKAiiwaFusionTrackMessageBufferP->push_back(oneKUKAiiwaFusionTrackMessage);
          }
          // Swaps the values.
          std::swap(m_receivedFrame, m_nextState);


      }
    }
  }

  void initHandles()
  {
    // Retrieves an vrep object handle based on its name.
    m_opticalTrackerBase = grl::vrep::getHandle(params_.OpticalTrackerBase);
    m_geometryIDToVrepMotionConfigMap = MotionConfigParamsToVrepHandleConfigMap(params_.MotionConfigParamsVector);
    allHandlesSet = true;
  }

private:
  Params params_;

  int m_opticalTrackerBase = -1;
  std::unique_ptr<grl::sensor::FusionTrack> opticalTrackerP;

  bool allHandlesSet = false;
  /// simple conditional for starting actually setting positions
  /// @see update() run_one()
  std::atomic<bool> isConnectionEstablished_;

  /// mutex that protects access of the main driver thread in update() from the separate vrep plugin messages thread
  /// it also protects the recording buffer when recording is being started, stopped, or cleared
  std::mutex m_frameAccess;

  /// the current frame available to the user, always acces after locking m_frameAccess
  std::unique_ptr<grl::sensor::FusionTrack::Frame> m_receivedFrame;
  /// the next frame state to access, always acces after locking m_frameAccess
  std::unique_ptr<grl::sensor::FusionTrack::Frame> m_nextState;
  /// builds up the file log in memory as data is received
  /// @todo TODO(ahundt) once using C++14 use unique_ptr https://stackoverflow.com/questions/8640393/move-capture-in-lambda
  std::shared_ptr<flatbuffers::FlatBufferBuilder> m_logFileBufferBuilderP;
  /// this is the current log data stored in memory
  /// @todo TODO(ahundt) once using C++14 use unique_ptr https://stackoverflow.com/questions/8640393/move-capture-in-lambda
  std::shared_ptr<std::vector<flatbuffers::Offset<grl::flatbuffer::KUKAiiwaFusionTrackMessage>>> m_KUKAiiwaFusionTrackMessageBufferP;
  /// should the driver stop collecting data from the atracsys devices
  std::atomic<bool> m_shouldStop;
  /// is data currently being recorded
  std::atomic<bool> m_isRecording;
  std::exception_ptr exceptionPtr;

  /// thread that polls the driver for new data and puts the data into the recording
  std::unique_ptr<std::thread> m_driverThread;
  /// @todo TODO(ahundt) the threads that saved files will build up forever, figure out how they can clear themselves out
  std::vector<std::shared_ptr<std::thread>> m_saveRecordingThreads;

  // note: elements match up with MotionConfigParams and MotionConfigParamsIndex, except here there are 3 and the main int is the geometryID.
  // The first int is the object that is being moved
  // The second int is the frame the object is being moved within
  // The third int is the object being measured by the optical tracker??.
  typedef std::tuple<int, int, int> VrepMotionConfigTuple;
  typedef std::map<int, VrepMotionConfigTuple> GeometryIDToVrepMotionConfigMap;

  GeometryIDToVrepMotionConfigMap m_geometryIDToVrepMotionConfigMap;

  /// Adds a configuration to to a config map
  static void MotionConfigParamsAddConfig(const MotionConfigParams &motionConfig, GeometryIDToVrepMotionConfigMap &IDToHandleConfig)
  {
   /// boost::lexical_cast, that can convert numbers from strings to numeric types like int or double and vice versa.
    IDToHandleConfig[boost::lexical_cast<int>(std::get<GeometryID>(motionConfig))] =
        /// Creates a tuple object, deducing the target type from the types of arguments.
        std::make_tuple(
            grl::vrep::getHandleFromParam<ObjectToMove>(motionConfig),
            grl::vrep::getHandleFromParam<FrameInWhichToMoveObject>(motionConfig),
            grl::vrep::getHandleFromParam<ObjectBeingMeasured>(motionConfig));
  }

  // converts the string identifiers for objects to integer handle identifiers
  // for use in updating the position of objects.
  template <typename InputIterator>
  GeometryIDToVrepMotionConfigMap
  MotionConfigParamsToVrepHandleConfigMap(const InputIterator &configurations)
  {
    GeometryIDToVrepMotionConfigMap IDToHandleConfig;
    for(auto &&motionConfig : configurations)
    {
      MotionConfigParamsAddConfig(motionConfig, IDToHandleConfig);
    }
    return IDToHandleConfig;
  }
};  /// End of class AtracsysFusionTrackVrepPlugin
}

#endif // _ATRACSYS_FUSION_TRACK_VREP_PLUGIN_HPP_
