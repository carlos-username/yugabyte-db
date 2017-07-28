// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.cloud;

import com.yugabyte.yw.models.helpers.DeviceInfo;

import java.util.List;
import java.util.UUID;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.yugabyte.yw.cloud.PublicCloudConstants.Tenancy;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.models.InstanceType;

public class ResourceUtil {
  public static final Logger LOG = LoggerFactory.getLogger(ResourceUtil.class);
  
  public static void mergeResourceDetails(DeviceInfo deviceInfo, 
  																				CloudType cloudType, 
  																				String instanceTypeCode, 
  																				String azCode,
  																				UniverseResourceDetails universeResourceDetails) {
    // Get the instance type object.
    InstanceType instanceType = InstanceType.get(cloudType.toString(), instanceTypeCode);
    if (instanceType == null) {
      String msg = "Couldn't find instance type " + instanceTypeCode + " for provider " +
          cloudType.toString();
      LOG.error(msg);
      throw new RuntimeException(msg);
    }
    universeResourceDetails.addVolumeCount(deviceInfo.numVolumes);
    universeResourceDetails.addVolumeSizeGB(deviceInfo.volumeSize * deviceInfo.numVolumes);
    universeResourceDetails.addMemSizeGB(instanceType.memSizeGB);
    universeResourceDetails.addAz(azCode);
    universeResourceDetails.addNumCores(instanceType.numCores);
  }
}
