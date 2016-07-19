// Copyright (c) YugaByte, Inc.

package controllers.commissioner.tasks;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.lang.reflect.Field;
import java.util.Iterator;
import java.util.Map.Entry;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.fasterxml.jackson.databind.JsonNode;

import controllers.commissioner.AbstractTaskBase;
import forms.commissioner.TaskParamsBase;
import models.commissioner.Universe;
import models.commissioner.Universe.NodeDetails;
import models.commissioner.Universe.UniverseDetails;
import models.commissioner.Universe.UniverseUpdater;
import play.libs.Json;
import util.Util;

public class AnsibleUpdateNodeInfo extends AbstractTaskBase {

  public static final Logger LOG = LoggerFactory.getLogger(AnsibleUpdateNodeInfo.class);

  public static class Params extends TaskParamsBase {}

  @Override
  public void run() {
    try {
      // Create the process to fetch information about the node from the cloud provider.
      String ybDevopsHome = Util.getDevopsHome();
      String command = ybDevopsHome + "/bin/find_cloud_host.sh" +
                       " " + taskParams.cloud +
                       " " + taskParams.nodeName +
                       " --json";
      LOG.info("Command to run: [{}]", command);

      // Run the process.
      Process p = Runtime.getRuntime().exec(command);

      // Read the stdout from the process, the result is printed out here.
      BufferedReader bout = new BufferedReader(new InputStreamReader(p.getInputStream()));
      // We expect a single line of json output.
      String line = bout.readLine();
      int exitValue = p.waitFor();
      LOG.info("Command [{}] finished with exit code {}.", command, exitValue);
      // TODO: log output stream somewhere.

      LOG.info("Updating details uuid={}, name={}.",
               taskParams.universeUUID, taskParams.nodeName);

      // Parse into a json object.
      final JsonNode jsonNode = Json.parse(line);
      // Update the node details and persist into the DB.
      UniverseUpdater updater = new UniverseUpdater() {
        @Override
        public void run(Universe universe) {
          // Get the details of the node to be updated.
          UniverseDetails universeDetails = universe.universeDetails;
          NodeDetails node = universeDetails.nodeDetailsMap.get(taskParams.nodeName);
          // Update each field of the node details based on the JSON output.
          Iterator<Entry<String, JsonNode>> iter = jsonNode.fields();
          while (iter.hasNext()) {
            Entry<String, JsonNode> entry = iter.next();
            Field field;
            try {
              LOG.info("Node " + taskParams.nodeName + ", setting field " + entry.getKey() +
                       " to value " + entry.getValue());
              // Error out if the host was not found.
              if (entry.getKey().equals("host_found") && entry.getValue().equals("false")) {
                throw new RuntimeException("Host " + taskParams.nodeName + " not found.");
              }
              field = NodeDetails.class.getField(entry.getKey());
              field.set(node, entry.getValue().asText());
            } catch (NoSuchFieldException | SecurityException e) {
              // We may not care about some fields, just warn and skip them.
              LOG.warn("Skipping field " + entry.getKey() + " with value " + entry.getValue());
            } catch (IllegalArgumentException e) {
              LOG.error("Field " + entry.getKey() + " could not be updated to value " +
                        entry.getValue(), e);
            } catch (IllegalAccessException e) {
              LOG.error("Field " + entry.getKey() + " could not be updated to value " +
                  entry.getValue(), e);
            }
          }
          // Update the node details.
          universeDetails.nodeDetailsMap.put(taskParams.nodeName, node);
        }
      };
      // Save the updated universe object.
      Universe.save(taskParams.universeUUID, updater);
    } catch (IOException e) {
      throw new RuntimeException(e);
    } catch (InterruptedException e) {
      throw new RuntimeException(e);
    }
  }
}
