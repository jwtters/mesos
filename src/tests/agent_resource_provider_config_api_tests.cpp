// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <mesos/type_utils.hpp>

#include <process/gtest.hpp>
#include <process/gmock.hpp>

#include <stout/fs.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>

#include "common/http.hpp"

#include "csi/paths.hpp"

#include "internal/evolve.hpp"

#include "slave/slave.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

namespace http = process::http;

using std::list;
using std::string;
using std::vector;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;
using process::PID;

using testing::Values;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

constexpr char TEST_SLRP_TYPE[] = "org.apache.mesos.rp.local.storage";
constexpr char TEST_SLRP_NAME[] = "test";


class AgentResourceProviderConfigApiTest
  : public ContainerizerTest<slave::MesosContainerizer>,
    public WithParamInterface<ContentType>
{
public:
  virtual void SetUp()
  {
    ContainerizerTest<slave::MesosContainerizer>::SetUp();

    resourceProviderConfigDir =
      path::join(sandbox.get(), "resource_provider_configs");

    ASSERT_SOME(os::mkdir(resourceProviderConfigDir));
  }

  ResourceProviderInfo createResourceProviderInfo(const string& volumes)
  {
    Option<ResourceProviderInfo> info;

    // This extra closure is necessary in order to use `ASSERT_*`, as
    // these macros require a void return type.
    [&]() {
      // Randomize the plugin name so we get a clean work directory for
      // each created config.
      const string testCsiPluginName =
        "test_csi_plugin_" +
        strings::remove(id::UUID::random().toString(), "-");

      const string testCsiPluginPath =
        path::join(tests::flags.build_dir, "src", "test-csi-plugin");

      const string testCsiPluginWorkDir =
        path::join(sandbox.get(), testCsiPluginName);
      ASSERT_SOME(os::mkdir(testCsiPluginWorkDir));

      Try<string> resourceProviderConfig = strings::format(
          R"~(
          {
            "type": "%s",
            "name": "%s",
            "default_reservations": [
              {
                "type": "DYNAMIC",
                "role": "storage"
              }
            ],
            "storage": {
              "plugin": {
                "type": "org.apache.mesos.csi.test",
                "name": "%s",
                "containers": [
                  {
                    "services": [
                      "CONTROLLER_SERVICE",
                      "NODE_SERVICE"
                    ],
                    "command": {
                      "shell": false,
                      "value": "%s",
                      "arguments": [
                        "%s",
                        "--available_capacity=0B",
                        "--volumes=%s",
                        "--work_dir=%s"
                      ]
                    }
                  }
                ]
              }
            }
          }
          )~",
          TEST_SLRP_TYPE,
          TEST_SLRP_NAME,
          testCsiPluginName,
          testCsiPluginPath,
          testCsiPluginPath,
          volumes,
          testCsiPluginWorkDir);

      ASSERT_SOME(resourceProviderConfig);

      Try<JSON::Object> json =
        JSON::parse<JSON::Object>(resourceProviderConfig.get());
      ASSERT_SOME(json);

      Try<ResourceProviderInfo> _info =
        ::protobuf::parse<ResourceProviderInfo>(json.get());
      ASSERT_SOME(_info);

      info = _info.get();
    }();

    return info.get();
  }

  virtual void TearDown()
  {
    foreach (const string& slaveWorkDir, slaveWorkDirs) {
      // Clean up CSI endpoint directories if there is any.
      const string csiRootDir = slave::paths::getCsiRootDir(slaveWorkDir);

      Try<list<string>> csiContainerPaths =
        csi::paths::getContainerPaths(csiRootDir, "*", "*");
      ASSERT_SOME(csiContainerPaths);

      foreach (const string& path, csiContainerPaths.get()) {
        Try<csi::paths::ContainerPath> containerPath =
          csi::paths::parseContainerPath(csiRootDir, path);
        ASSERT_SOME(containerPath);

        Result<string> endpointDir =
          os::realpath(csi::paths::getEndpointDirSymlinkPath(
              csiRootDir,
              containerPath->type,
              containerPath->name,
              containerPath->containerId));

        if (endpointDir.isSome()) {
          ASSERT_SOME(os::rmdir(endpointDir.get()));
        }
      }
    }

    ContainerizerTest<slave::MesosContainerizer>::TearDown();
  }

  virtual slave::Flags CreateSlaveFlags()
  {
    slave::Flags flags =
      ContainerizerTest<slave::MesosContainerizer>::CreateSlaveFlags();

    // Store the agent work directory for cleaning up CSI endpoint
    // directories during teardown.
    // NOTE: DO NOT change the work directory afterward.
    slaveWorkDirs.push_back(flags.work_dir);

    return flags;
  }

  Future<http::Response> addResourceProviderConfig(
      const PID<Slave>& pid,
      const ContentType& contentType,
      const ResourceProviderInfo& info)
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    agent::Call call;
    call.set_type(agent::Call::ADD_RESOURCE_PROVIDER_CONFIG);
    call.mutable_add_resource_provider_config()
      ->mutable_info()->CopyFrom(info);

    return http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, evolve(call)),
        stringify(contentType));
  }

  Future<http::Response> updateResourceProviderConfig(
      const PID<Slave>& pid,
      const ContentType& contentType,
      const ResourceProviderInfo& info)
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    agent::Call call;
    call.set_type(agent::Call::UPDATE_RESOURCE_PROVIDER_CONFIG);
    call.mutable_update_resource_provider_config()
      ->mutable_info()->CopyFrom(info);

    return http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, evolve(call)),
        stringify(contentType));
  }

  Future<http::Response> removeResourceProviderConfig(
      const PID<Slave>& pid,
      const ContentType& contentType,
      const string& type,
      const string& name)
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    agent::Call call;
    call.set_type(agent::Call::REMOVE_RESOURCE_PROVIDER_CONFIG);
    call.mutable_remove_resource_provider_config()->set_type(type);
    call.mutable_remove_resource_provider_config()->set_name(name);

    return http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, evolve(call)),
        stringify(contentType));
  }

protected:
  vector<string> slaveWorkDirs;
  string resourceProviderConfigDir;
};


// The tests are parameterized by the content type of the request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    AgentResourceProviderConfigApiTest,
    Values(ContentType::PROTOBUF, ContentType::JSON));


// This test adds a new resource provider config on the fly.
TEST_P(AgentResourceProviderConfigApiTest, ROOT_Add)
{
  const ContentType contentType = GetParam();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to wait for an offer having the provider
  // resource.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(&Resources::hasResourceProvider, lambda::_1))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // Add a new resource provider.
  ResourceProviderInfo info = createResourceProviderInfo("volume1:4GB");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::OK().status,
      addResourceProviderConfig(slave.get()->pid, contentType, info));

  // Check that a new config file is created.
  Try<list<string>> configPaths =
    fs::list(path::join(resourceProviderConfigDir, "*"));
  ASSERT_SOME(configPaths);
  EXPECT_EQ(1u, configPaths->size());

  Try<string> read = os::read(configPaths->back());
  ASSERT_SOME(read);

  Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
  ASSERT_SOME(json);

  Try<ResourceProviderInfo> _info =
    ::protobuf::parse<ResourceProviderInfo>(json.get());
  ASSERT_SOME(_info);
  EXPECT_EQ(_info.get(), info);

  // Wait for an offer having the provider resource.
  AWAIT_READY(offers);
}


// This test checks that adding a resource provider config that already
// exists is not allowed.
TEST_P(AgentResourceProviderConfigApiTest, ROOT_AddConflict)
{
  const ContentType contentType = GetParam();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  // Generate a pre-existing config.
  const string configPath = path::join(resourceProviderConfigDir, "test.json");
  ASSERT_SOME(os::write(
      configPath,
      stringify(JSON::protobuf(createResourceProviderInfo("volume1:4GB")))));

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  ResourceProviderInfo info = createResourceProviderInfo("volume1:2GB");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::Conflict().status,
      addResourceProviderConfig(slave.get()->pid, contentType, info));

  // Check that no new config is created, and the existing one is not
  // overwritten.
  Try<list<string>> configPaths =
    fs::list(path::join(resourceProviderConfigDir, "*"));
  ASSERT_SOME(configPaths);
  EXPECT_EQ(1u, configPaths->size());
  EXPECT_EQ(configPath, configPaths->back());

  Try<string> read = os::read(configPath);
  ASSERT_SOME(read);

  Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
  ASSERT_SOME(json);

  Try<ResourceProviderInfo> _info =
    ::protobuf::parse<ResourceProviderInfo>(json.get());
  ASSERT_SOME(_info);
  EXPECT_NE(_info.get(), info);
}


// This test updates an existing resource provider config on the fly.
TEST_P(AgentResourceProviderConfigApiTest, ROOT_Update)
{
  const ContentType contentType = GetParam();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  // Generate a pre-existing config.
  const string configPath = path::join(resourceProviderConfigDir, "test.json");
  ASSERT_SOME(os::write(
      configPath,
      stringify(JSON::protobuf(createResourceProviderInfo("volume1:4GB")))));

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to wait for an offer having the provider
  // resource.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DeclineOffers(declineFilters));

  Future<vector<Offer>> oldOffers;

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(&Resources::hasResourceProvider, lambda::_1))))
    .WillOnce(FutureArg<1>(&oldOffers));

  driver.start();

  // Wait for an offer having the old provider resource.
  AWAIT_READY(oldOffers);
  ASSERT_FALSE(oldOffers->empty());

  Future<OfferID> rescinded;

  EXPECT_CALL(sched, offerRescinded(&driver, oldOffers->at(0).id()))
    .WillOnce(FutureArg<1>(&rescinded));

  Future<vector<Offer>> newOffers;

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(&Resources::hasResourceProvider, lambda::_1))))
    .WillOnce(FutureArg<1>(&newOffers));

  ResourceProviderInfo info = createResourceProviderInfo("volume1:2GB");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::OK().status,
      updateResourceProviderConfig(slave.get()->pid, contentType, info));

  // Check that no new config is created, and the existing one is overwritten.
  Try<list<string>> configPaths =
    fs::list(path::join(resourceProviderConfigDir, "*"));
  ASSERT_SOME(configPaths);
  EXPECT_EQ(1u, configPaths->size());
  EXPECT_EQ(configPath, configPaths->back());

  Try<string> read = os::read(configPath);
  ASSERT_SOME(read);

  Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
  ASSERT_SOME(json);

  Try<ResourceProviderInfo> _info =
    ::protobuf::parse<ResourceProviderInfo>(json.get());
  ASSERT_SOME(_info);
  EXPECT_EQ(_info.get(), info);

  // Wait for the old offer to be rescinded.
  AWAIT_READY(rescinded);

  // Wait for an offer having the new provider resource.
  AWAIT_READY(newOffers);

  // The new provider resource is smaller than the old provider resource.
  EXPECT_FALSE(Resources(newOffers->at(0).resources()).contains(
      oldOffers->at(0).resources()));
}


// This test checks that updating a nonexistent resource provider config
// is not allowed.
TEST_P(AgentResourceProviderConfigApiTest, UpdateNotFound)
{
  const ContentType contentType = GetParam();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  ResourceProviderInfo info = createResourceProviderInfo("volume1:4GB");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::NotFound().status,
      updateResourceProviderConfig(slave.get()->pid, contentType, info));

  // Check that no new config is created.
  Try<list<string>> configPaths =
    fs::list(path::join(resourceProviderConfigDir, "*"));
  ASSERT_SOME(configPaths);
  EXPECT_TRUE(configPaths->empty());
}


// This test removes an existing resource provider config on the fly.
TEST_P(AgentResourceProviderConfigApiTest, ROOT_Remove)
{
  const ContentType contentType = GetParam();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  // Generate a pre-existing config.
  const string configPath = path::join(resourceProviderConfigDir, "test.json");
  ResourceProviderInfo info = createResourceProviderInfo("volume1:4GB");
  ASSERT_SOME(os::write(configPath, stringify(JSON::protobuf(info))));

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to wait for an offer having the provider
  // resource.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DeclineOffers(declineFilters));

  Future<vector<Offer>> oldOffers;

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(&Resources::hasResourceProvider, lambda::_1))))
    .WillOnce(FutureArg<1>(&oldOffers));

  driver.start();

  // Wait for an offer having the old provider resource.
  AWAIT_READY(oldOffers);
  ASSERT_FALSE(oldOffers->empty());

  Future<OfferID> rescinded;

  EXPECT_CALL(sched, offerRescinded(&driver, oldOffers->at(0).id()))
    .WillOnce(FutureArg<1>(&rescinded));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::OK().status,
      removeResourceProviderConfig(
          slave.get()->pid, contentType, info.type(), info.name()));

  // Check that the existing config is removed.
  EXPECT_FALSE(os::exists(configPath));

  // Wait for the old offer to be rescinded.
  AWAIT_READY(rescinded);
}


// This test checks that removing a nonexistent resource provider config
// is not allowed.
TEST_P(AgentResourceProviderConfigApiTest, RemoveNotFound)
{
  const ContentType contentType = GetParam();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  ResourceProviderInfo info = createResourceProviderInfo("volume1:4GB");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::NotFound().status,
      removeResourceProviderConfig(
          slave.get()->pid, contentType, info.type(), info.name()));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
