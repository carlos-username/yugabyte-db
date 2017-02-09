// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { DescriptionList } from '../../common/descriptors';
import { ROOT_URL } from '../../../config';
import './connectStringPanel.css';

export default class ConnectStringPanel extends Component {
  render() {
    const { universeInfo, customerId } = this.props;
    const { universeDetails } = universeInfo;
    const { userIntent } = universeDetails;
    var universeId = universeInfo.universeUUID;
    const endpointUrl = ROOT_URL + "/customers/" + customerId +
                        "/universes/" + universeId + "/masters";
    const connect_string = "yb_load_test_tool --load_test_master_endpoint " + endpointUrl;
    const endpoint =
      <a href={endpointUrl} target="_blank">Endpoint &nbsp;
        <i className="fa fa-external-link" aria-hidden="true"></i>
      </a>;
    var connectStringPanelItems = [
      {name: "Nodes", data: userIntent.numNodes},
      {name: "Replication Factor", data: userIntent.replicationFactor},
      {name: "Meta Masters", data: endpoint},
      {name: "Load Test", data: connect_string, dataClass: "yb-code-snippet well well-sm"}
    ];
    return (
      <DescriptionList listItems={connectStringPanelItems} />
    );
  }
}
