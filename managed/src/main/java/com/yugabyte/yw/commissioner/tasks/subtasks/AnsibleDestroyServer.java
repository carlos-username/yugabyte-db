// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks.subtasks;

import com.yugabyte.yw.common.NodeManager;
import com.yugabyte.yw.common.ShellProcessHandler;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.NodeInstance;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Universe.UniverseUpdater;
import com.yugabyte.yw.models.helpers.NodeDetails;


public class AnsibleDestroyServer extends NodeTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(AnsibleDestroyServer.class);

  // Params for this task.
  public static class Params extends NodeTaskParams {}

  @Override
  protected Params taskParams() {
    return (Params)taskParams;
  }

  private void removeNodeFromUniverse(String nodeName) {
    // Persist the desired node information into the DB.
    UniverseUpdater updater = new UniverseUpdater() {
      @Override
      public void run(Universe universe) {
        UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
        universeDetails.nodeDetailsSet.remove(nodeName);
        LOG.debug("Removing node " + nodeName +
                  " from universe " + taskParams().universeUUID);
      }
    };

    Universe.saveDetails(taskParams().universeUUID, updater);

    if (taskParams().cloud == Common.CloudType.onprem) {
      // Free up the node.
      NodeInstance node = NodeInstance.getByName(nodeName);
      node.inUse = false;
      node.save();
    }
  }

  @Override
  public void run() {
    // Update the node state as being decommissioned.
    setNodeState(NodeDetails.NodeState.BeingDecommissioned);
    // Execute the ansible command.
    ShellProcessHandler.ShellResponse response = getNodeManager().nodeCommand(
        NodeManager.NodeCommandType.Destroy, taskParams());
    logShellResponse(response);

    // Update the node state to destroyed. Even though we remove the node below, this will
    // help tracking state for any nodes stuck in limbo.
    setNodeState(NodeDetails.NodeState.Destroyed);

    removeNodeFromUniverse(taskParams().nodeName);
  }
}
