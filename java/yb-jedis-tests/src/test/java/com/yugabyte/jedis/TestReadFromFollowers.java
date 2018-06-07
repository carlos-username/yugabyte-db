// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
package com.yugabyte.jedis;

import com.google.protobuf.ByteString;
import org.apache.commons.text.RandomStringGenerator;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.Parameterized;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.Common;
import org.yb.client.GetTableSchemaResponse;
import org.yb.client.ModifyClusterConfigLiveReplicas;
import org.yb.client.TestUtils;
import org.yb.client.YBClient;
import org.yb.master.Master;
import redis.clients.jedis.HostAndPort;
import redis.clients.jedis.Jedis;
import redis.clients.jedis.YBJedis;

import java.net.InetSocketAddress;
import java.util.*;
import java.util.stream.Collectors;

import static junit.framework.TestCase.assertEquals;
import static junit.framework.TestCase.assertNull;
import static junit.framework.TestCase.assertTrue;

@RunWith(Parameterized.class)
public class TestReadFromFollowers extends BaseJedisTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestYBJedis.class);

  protected static final int WAIT_FOR_FOLLOWER_TO_CATCH_UP_TIMEOUT_MS = 60000; // 60 seconds.

  public TestReadFromFollowers(JedisClientType jedisClientType) {
    super(jedisClientType);
  }

  public int getTestMethodTimeoutSec() {
    return 360;
  }

  // Run each test with both Jedis and YBJedis clients.
  @Parameterized.Parameters
  public static Collection jedisClients() {
    return Arrays.asList(JedisClientType.JEDIS, JedisClientType.YBJEDIS);
  }

  @Override
  public void setUpBefore() throws Exception {
    TestUtils.clearReservedPorts();
  }

  @Override
  public void setUpJedis() throws Exception {
    if (miniCluster == null) {
      return;
    }

    // Create the redis table.
    miniCluster.getClient().createRedisTable(YBClient.REDIS_DEFAULT_TABLE_NAME);

    GetTableSchemaResponse tableSchema = miniCluster.getClient().getTableSchema(
        YBClient.REDIS_KEYSPACE_NAME, YBClient.REDIS_DEFAULT_TABLE_NAME);

    assertEquals(Common.PartitionSchemaPB.HashSchema.REDIS_HASH_SCHEMA,
        tableSchema.getPartitionSchema().getHashSchema());

    // Setup the Jedis client.
    List<InetSocketAddress> redisContactPoints = miniCluster.getRedisContactPoints();

    switch (jedisClientType) {
      case JEDIS:
        LOG.info("Connecting to: " + redisContactPoints.get(0).toString());
        jedis_client = new Jedis(redisContactPoints.get(0).getHostName(),
            redisContactPoints.get(0).getPort(), JEDIS_SOCKET_TIMEOUT_MS);
        break;
      case YBJEDIS:
        LOG.info("Connecting to: " + redisContactPoints.get(0).toString());
        jedis_client = new YBJedis(redisContactPoints.get(0).getHostName(),
            redisContactPoints.get(0).getPort(), JEDIS_SOCKET_TIMEOUT_MS);
        break;
    }
  }

  public void testReadsSucceed() throws Exception {
    RandomStringGenerator generator = new RandomStringGenerator.Builder()
        .withinRange('a', 'z').build();

    for (int i = 0; i < 500; i++) {
      String s = generator.generate(20);
      LOG.info(String.format("Iteration %d. Setting and getting %s", i, s));
      assertEquals("OK", jedis_client.set(s, "v"));
      TestUtils.waitFor(() -> {
        if (jedis_client.get(s) == null) {
          return false;
        }
        return true;
      }, WAIT_FOR_FOLLOWER_TO_CATCH_UP_TIMEOUT_MS, 100);
      assertEquals("v", jedis_client.get(s));
    }
  }

  @Test
  public void testLocalOps() throws Exception {
    assertNull(miniCluster);

    // We don't want the tablets to move while we are testing because Jedis will be left with a
    // stale key partition map since it won't receive a MOVED request which triggers the update.
    List<String> masterArgs = Arrays.asList("--enable_load_balancing=false");

    String tserverRedisFollowerFlag = "--redis_allow_reads_from_followers=true";
    String tserverAssertLocalTablet = "--assert_local_tablet_server_selected=true";
    List<List<String>> tserverArgs = new ArrayList<List<String>>();
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertLocalTablet));
    createMiniCluster(3, masterArgs, tserverArgs);

    // Setup the Jedis client.
    setUpJedis();

    testReadsSucceed();
  }

  @Test
  public void testSameZoneOps() throws Exception {
    assertNull(miniCluster);

    final String PLACEMENT_CLOUD = "testCloud";
    final String PLACEMENT_REGION = "testRegion";
    final String PLACEMENT_ZONE0 = "testZone0";
    final String PLACEMENT_ZONE1 = "testZone1";
    final String PLACEMENT_ZONE2 = "testZone2";
    final String PLACEMENT_UUID = "placementUuid";

    // We don't want the tablets to move while we are testing because Jedis will be left with a
    // stale key partition map since it won't receive a MOVED request which triggers the update.
    List<String> masterArgs = Arrays.asList("--enable_load_balancing=false");

    String tserverRedisFollowerFlag = "--redis_allow_reads_from_followers=true";
    String tserverAssertTSInZone = "--assert_tablet_server_select_is_in_zone=testZone0";
    List<List<String>> tserverArgs = new ArrayList<List<String>>();

    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE0, "--placement_uuid=" + PLACEMENT_UUID));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE0, "--placement_uuid=" + PLACEMENT_UUID));

    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE1, "--placement_uuid=" + PLACEMENT_UUID));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE1, "--placement_uuid=" + PLACEMENT_UUID));

    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE2, "--placement_uuid=" + PLACEMENT_UUID));
    tserverArgs.add(Arrays.asList(tserverRedisFollowerFlag, tserverAssertTSInZone,
        "--placement_cloud=" + PLACEMENT_CLOUD, "--placement_region=" + PLACEMENT_REGION,
        "--placement_zone=" + PLACEMENT_ZONE2, "--placement_uuid=" + PLACEMENT_UUID));

    createMiniCluster(3, masterArgs, tserverArgs);

    YBClient syncClient = miniCluster.getClient();

    // Create the cluster config pb to be sent to the masters
    org.yb.WireProtocol.CloudInfoPB cloudInfo0 = org.yb.WireProtocol.CloudInfoPB.newBuilder()
        .setPlacementCloud(PLACEMENT_CLOUD)
        .setPlacementRegion(PLACEMENT_REGION)
        .setPlacementZone(PLACEMENT_ZONE0)
        .build();

    // Create the cluster config pb to be sent to the masters
    org.yb.WireProtocol.CloudInfoPB cloudInfo1 = org.yb.WireProtocol.CloudInfoPB.newBuilder()
        .setPlacementCloud(PLACEMENT_CLOUD)
        .setPlacementRegion(PLACEMENT_REGION)
        .setPlacementZone(PLACEMENT_ZONE1)
        .build();

    // Create the cluster config pb to be sent to the masters
    org.yb.WireProtocol.CloudInfoPB cloudInfo2 = org.yb.WireProtocol.CloudInfoPB.newBuilder()
        .setPlacementCloud(PLACEMENT_CLOUD)
        .setPlacementRegion(PLACEMENT_REGION)
        .setPlacementZone(PLACEMENT_ZONE2)
        .build();

    Master.PlacementBlockPB placementBlock0 =
        Master.PlacementBlockPB.newBuilder().setCloudInfo(cloudInfo0).setMinNumReplicas(1).build();

    Master.PlacementBlockPB placementBlock1 =
        Master.PlacementBlockPB.newBuilder().setCloudInfo(cloudInfo1).setMinNumReplicas(1).build();

    Master.PlacementBlockPB placementBlock2 =
        Master.PlacementBlockPB.newBuilder().setCloudInfo(cloudInfo2).setMinNumReplicas(1).build();

    List<Master.PlacementBlockPB> placementBlocksLive = new ArrayList<Master.PlacementBlockPB>();
    placementBlocksLive.add(placementBlock0);
    placementBlocksLive.add(placementBlock1);
    placementBlocksLive.add(placementBlock2);


    Master.PlacementInfoPB livePlacementInfo =
        Master.PlacementInfoPB.newBuilder().addAllPlacementBlocks(placementBlocksLive).
            setPlacementUuid(ByteString.copyFromUtf8(PLACEMENT_UUID)).build();

    ModifyClusterConfigLiveReplicas liveOperation =
        new ModifyClusterConfigLiveReplicas(syncClient, livePlacementInfo);
    try {
      liveOperation.doCall();
    } catch (Exception e) {
      LOG.warn("Failed with error:", e);
      assertTrue(false);
    }

    // Setup the Jedis client.
    setUpJedis();

    testReadsSucceed();
  }
}
