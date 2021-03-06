#include "CommandIsolator.hpp"
#include "CommandRunner.hpp"
#include "Helpers.hpp"
#include "Logger.hpp"

#include <glog/logging.h>
#include <process/after.hpp>
#include <process/dispatch.hpp>
#include <process/loop.hpp>
#include <process/process.hpp>
#include <process/time.hpp>

namespace criteo {
namespace mesos {

using process::Clock;
using std::string;

using ::mesos::ContainerID;
using ::mesos::slave::ContainerConfig;
using ::mesos::slave::ContainerLaunchInfo;
using ::mesos::slave::ContainerLimitation;

using process::Break;
using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::loop;
using process::after;

class CommandIsolatorProcess : public process::Process<CommandIsolatorProcess> {
 public:
  CommandIsolatorProcess(const Option<Command>& prepareCommand,
                         const Option<RecurrentCommand>& watchCommand,
                         const Option<Command>& cleanupCommand,
                         const Option<Command>& usageCommand, bool isDebugMode);

  virtual process::Future<Option<ContainerLaunchInfo>> prepare(
      const ContainerID& containerId, const ContainerConfig& containerConfig);

  virtual process::Future<ContainerLimitation> watch(
      const ContainerID& containerId);

  virtual process::Future<Nothing> cleanup(const ContainerID& containerId);

  virtual process::Future<::mesos::ResourceStatistics> usage(
      const ContainerID& containerId);

  inline const Option<Command>& prepareCommand() const {
    return m_prepareCommand;
  }

  inline const Option<Command>& cleanupCommand() const {
    return m_cleanupCommand;
  }

 private:
  inline static ::mesos::ResourceStatistics emptyStats(
      double timestamp = Clock::now().secs()) {
    ::mesos::ResourceStatistics stats;
    stats.set_timestamp(timestamp);
    return stats;
  }

  Option<Command> m_prepareCommand;
  Option<RecurrentCommand> m_watchCommand;
  Option<Command> m_cleanupCommand;
  Option<Command> m_usageCommand;
  bool m_isDebugMode;
  hashmap<ContainerID, ContainerConfig> m_infos;
};

CommandIsolatorProcess::CommandIsolatorProcess(
    const Option<Command>& prepareCommand,
    const Option<RecurrentCommand>& watchCommand,
    const Option<Command>& cleanupCommand, const Option<Command>& usageCommand,
    bool isDebugMode)
    : m_prepareCommand(prepareCommand),
      m_watchCommand(watchCommand),
      m_cleanupCommand(cleanupCommand),
      m_usageCommand(usageCommand),
      m_isDebugMode(isDebugMode) {}

process::Future<Option<ContainerLaunchInfo>> CommandIsolatorProcess::prepare(
    const ContainerID& containerId, const ContainerConfig& containerConfig) {
  if (m_infos.contains(containerId)) {
    return Failure("mesos-command-module already initialized for container");
  } else {
    m_infos.put(containerId, containerConfig);
  }
  if (m_prepareCommand.isNone()) {
    return None();
  }

  logging::Metadata metadata = {containerId.value(), "prepare"};

  JSON::Object inputsJson;
  inputsJson.values["container_id"] = JSON::protobuf(containerId);
  inputsJson.values["container_config"] = JSON::protobuf(containerConfig);

  Try<string> output = CommandRunner(m_isDebugMode, metadata)
                           .run(m_prepareCommand.get(), stringify(inputsJson));

  if (output.isError()) {
    return Failure(output.error());
  }

  if (output->empty()) {
    return None();
  }

  Result<ContainerLaunchInfo> containerLaunchInfo =
      jsonToProtobuf<ContainerLaunchInfo>(output.get());

  if (containerLaunchInfo.isError()) {
    return Failure("Unable to deserialize ContainerLaunchInfo: " +
                   containerLaunchInfo.error());
  }
  return containerLaunchInfo.get();
}

process::Future<ContainerLimitation> CommandIsolatorProcess::watch(
    const ContainerID& containerId) {
  if (m_watchCommand.isNone()) {
    return process::Future<ContainerLimitation>();
  }

  logging::Metadata metadata = {containerId.value(), "watch"};

  JSON::Object inputsJson;
  inputsJson.values["container_id"] = JSON::protobuf(containerId);
  if (m_infos.contains(containerId)) {
    inputsJson.values["container_config"] =
        JSON::protobuf(m_infos[containerId]);
  } else {
    return Failure(
        "mesos-command-module is not initialized for current container");
  }

  std::string inputStringified = stringify(inputsJson);
  RecurrentCommand command = m_watchCommand.get();
  bool isDebugMode = m_isDebugMode;

  process::UPID proc = spawn(new process::ProcessBase());

  Future<ContainerLimitation> future = loop(
      proc,
      [isDebugMode, metadata, inputStringified, command]() {
        Try<string> output = CommandRunner(isDebugMode, metadata)
                                 .runWithoutTimeout(command, inputStringified);
        return output;
      },
      [command, this, containerId](
          Try<string> output) -> Future<ControlFlow<ContainerLimitation>> {
        try {
          if (!m_infos.contains(containerId)) {
            LOG(WARNING) << "Terminating watch loop for containerId: "
                         << containerId;
            // Returning a discarded future stops the loop
            Future<ControlFlow<ContainerLimitation>> ret;
            ret.discard();
            return ret;
          }
          if (output.isError())
            throw std::runtime_error("Unable to parse output: " +
                                     output.error());

          if (output->empty()) throw should_continue_exception();

          Result<ContainerLimitation> containerLimitation =
              jsonToProtobuf<ContainerLimitation>(output.get());

          if (containerLimitation.isError())
            throw std::runtime_error(
                "Unable to deserialize ContainerLimitation: " +
                containerLimitation.error());

          return Break(containerLimitation.get());
        } catch (const std::runtime_error& e) {
          if (e.what()) LOG(WARNING) << e.what();
        } catch (const should_continue_exception& e) {
        }
        return after(Seconds(command.frequence()))
            .then([]() -> process::ControlFlow<ContainerLimitation> {
              return process::Continue();
            });
      });

  future.onAny([proc]() {
    LOG(WARNING) << "Terminating watch loop";
    terminate(proc);
    LOG(WARNING) << "Watch loop Terminated";
  });

  return future;
}

process::Future<::mesos::ResourceStatistics> CommandIsolatorProcess::usage(
    const ContainerID& containerId) {
  double now = Clock::now().secs();

  if (m_usageCommand.isNone()) return emptyStats(now);

  logging::Metadata metadata = {containerId.value(), "usage"};

  JSON::Object inputsJson;
  inputsJson.values["container_id"] = JSON::protobuf(containerId);
  if (m_infos.contains(containerId)) {
    inputsJson.values["container_config"] =
        JSON::protobuf(m_infos[containerId]);
  } else {
    return Failure(
        "mesos-command-module is not initialized for current container");
  }

  return CommandRunner(m_isDebugMode, metadata)
      .asyncRun(m_usageCommand.get(), stringify(inputsJson))
      .then([now = now](Try<string> output)
                ->Future<::mesos::ResourceStatistics> {
                  if (output.isError()) {
                    LOG(WARNING) << "Unable to parse output: "
                                 << output.error();
                    return emptyStats(now);
                  }
                  if (output->empty()) {
                    LOG(WARNING) << "Output is empty";
                    return emptyStats(now);
                  }
                  Result<::mesos::ResourceStatistics> resourceStatistics =
                      jsonToProtobuf<::mesos::ResourceStatistics>(output.get());

                  if (resourceStatistics.isError()) {
                    LOG(WARNING) << "Unable to deserialize ResourceStatistics: "
                                 << resourceStatistics.error();
                    return emptyStats(now);
                  }
                  return resourceStatistics.get();
                })
      .recover([now = now](const Future<::mesos::ResourceStatistics>& result)
                   ->Future<::mesos::ResourceStatistics> {
                     LOG(WARNING) << "Failed to run usage command: " << result;
                     return emptyStats(now);
                   });
}

process::Future<Nothing> CommandIsolatorProcess::cleanup(
    const ContainerID& containerId) {
  if (m_cleanupCommand.isNone()) {
    m_infos.erase(containerId);
    return Nothing();
  }

  logging::Metadata metadata = {containerId.value(), "cleanup"};

  JSON::Object inputsJson;
  inputsJson.values["container_id"] = JSON::protobuf(containerId);
  if (m_infos.contains(containerId)) {
    inputsJson.values["container_config"] =
        JSON::protobuf(m_infos[containerId]);
  } else {
    LOG(WARNING)
        << "Missing container info during cleanup of mesos-command-module.";
  }

  Try<string> output = CommandRunner(m_isDebugMode, metadata)
                           .run(m_cleanupCommand.get(), stringify(inputsJson));

  m_infos.erase(containerId);
  if (output.isError()) {
    return Failure(output.error());
  }

  return Nothing();
}

CommandIsolator::CommandIsolator(const Option<Command>& prepareCommand,
                                 const Option<RecurrentCommand>& watchCommand,
                                 const Option<Command>& cleanupCommand,
                                 const Option<Command>& usageCommand,
                                 bool isDebugMode)
    : m_process(new CommandIsolatorProcess(prepareCommand, watchCommand,
                                           cleanupCommand, usageCommand,
                                           isDebugMode)) {
  spawn(m_process);
}

CommandIsolator::~CommandIsolator() {
  if (m_process != nullptr) {
    terminate(m_process);
    wait(m_process);
    delete m_process;
  }
}

process::Future<Option<ContainerLaunchInfo>> CommandIsolator::prepare(
    const ContainerID& containerId, const ContainerConfig& containerConfig) {
  return dispatch(m_process, &CommandIsolatorProcess::prepare, containerId,
                  containerConfig);
}

process::Future<ContainerLimitation> CommandIsolator::watch(
    const ContainerID& containerId) {
  return dispatch(m_process, &CommandIsolatorProcess::watch, containerId);
}

process::Future<Nothing> CommandIsolator::cleanup(
    const ContainerID& containerId) {
  return dispatch(m_process, &CommandIsolatorProcess::cleanup, containerId);
}

process::Future<::mesos::ResourceStatistics> CommandIsolator::usage(
    const ContainerID& containerId) {
  return dispatch(m_process, &CommandIsolatorProcess::usage, containerId);
}

const Option<Command>& CommandIsolator::prepareCommand() const {
  CHECK_NOTNULL(m_process);
  return m_process->prepareCommand();
}

const Option<Command>& CommandIsolator::cleanupCommand() const {
  CHECK_NOTNULL(m_process);
  return m_process->cleanupCommand();
}
}  // namespace mesos
}  // namespace criteo
