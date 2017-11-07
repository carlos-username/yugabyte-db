// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Field, FieldArray } from 'redux-form';
import { Row, Col, Tabs, Tab } from 'react-bootstrap';
import { YBButton, YBModal, YBInputField, YBSelectWithLabel } from '../fields';
import { isNonEmptyArray } from 'utils/ObjectUtils';
import './RollingUpgradeForm.scss';

class FlagInput extends Component {
  render() {
    const {deleteRow, item} = this.props;
    return (
      <Row>
        <Col lg={5}>
          <Field name={`${item}.name`} component={YBInputField} className="input-sm" placeHolder="GFlag Name"/>
        </Col>
        <Col lg={5}>
          <Field name={`${item}.value`} component={YBInputField} className="input-sm" placeHolder="Value"/>
        </Col>
        <Col lg={1}>
          <i className="fa fa-times fa-fw delete-row-btn" onClick={deleteRow}/>
        </Col>
      </Row>
    );
  }
}

class FlagItems extends Component {
  componentWillMount() {
    if (this.props.fields.length === 0) {
      this.props.fields.push({});
    }
  }
  render() {
    const { fields } = this.props;
    const addFlagItem = function() {
      fields.push({});
    };
    const gFlagsFieldList = fields.map((item, idx) => (
      <FlagInput item={item} key={idx} deleteRow={() => fields.remove(idx)} />
    ));

    return (
      <div>
        {
          gFlagsFieldList
        }
        <YBButton btnClass="btn btn-sm universe-btn btn-default bg-orange"
                  btnText="Add" btnIcon="fa fa-plus"
                  onClick={addFlagItem} />
      </div>
    );
  }
}


export default class RollingUpgradeForm extends Component {
  constructor(props) {
    super(props);
    this.setRollingUpgradeProperties = this.setRollingUpgradeProperties.bind(this);
  }

  setRollingUpgradeProperties(values) {
    const { universe: {visibleModal, currentUniverse: {data: {universeDetails: {userIntent}, universeUUID}}}, reset} = this.props;
    const payload = {};
    if (visibleModal === "softwareUpgradesModal") {
      payload.taskType = "Software";
    } else if (visibleModal === "gFlagsModal") {
      payload.taskType = "GFlags";
    } else {
      return;
    }
    payload.ybSoftwareVersion = values.ybSoftwareVersion;
    payload.universeUUID = universeUUID;
    payload.userIntent = userIntent;
    let masterGFlagList = [];
    let tserverGFlagList = [];
    if (isNonEmptyArray(values.masterGFlags)) {
      masterGFlagList = values.masterGFlags.map(function(masterFlag, masterIdx){
        if (masterFlag.name && masterFlag.value) {
          return {name: masterFlag.name, value: masterFlag.value};
        } else {
          return null;
        }
      }).filter(Boolean);
    }
    if (isNonEmptyArray(values.tserverGFlags)) {
      tserverGFlagList = values.tserverGFlags.map(function(tserverFlag, tserverIdx){
        if (tserverFlag.name && tserverFlag.value) {
          return {name: tserverFlag.name, value: tserverFlag.value};
        } else {
          return null;
        }
      }).filter(Boolean);
    }
    payload.masterGFlags = masterGFlagList;
    payload.tserverGFlags = tserverGFlagList;
    payload.sleepAfterMasterRestartMillis = values.timeDelay * 1000;
    payload.sleepAfterTServerRestartMillis = values.timeDelay * 1000;
    this.props.submitRollingUpgradeForm(payload, universeUUID, reset);
  }

  render() {
    const self = this;
    const {onHide, modalVisible, handleSubmit, universe: {visibleModal,
      error}, resetRollingUpgrade, softwareVersions} = this.props;

    const submitAction = handleSubmit(self.setRollingUpgradeProperties);
    let title = "";
    let formBody = <span/>;
    const softwareVersionOptions = softwareVersions.map(function(item, idx){
      return <option key={idx} value={item}>{item}</option>;
    });
    const formCloseAction = function() {
      onHide();
      self.props.reset();
    };
    if (visibleModal === "softwareUpgradesModal") {
      title="Upgrade Software";
      formBody = (
        <span>
          <Col lg={12} className="form-section-title">
            Software Package Version
          </Col>
          <Field name="ybSoftwareVersion" type="select" component={YBSelectWithLabel}
            options={softwareVersionOptions} label="Server Version"
            onInputChanged={this.softwareVersionChanged}/>
        </span>
      );
    } else {
      title = "GFlags";
      formBody = (
        <div>
          <Tabs defaultActiveKey={2} className="gflag-display-container" id="gflag-container" >
            <Tab eventKey={1} title="Master" className="gflag-class-1" bsClass="gflag-class-2">
              <FieldArray name="masterGFlags" component={FlagItems} resetRollingUpgrade={resetRollingUpgrade}/>
            </Tab>
            <Tab eventKey={2} title="T-Server">
              <FieldArray name="tserverGFlags" component={FlagItems} resetRollingUpgrade={resetRollingUpgrade}/>
            </Tab>
          </Tabs>
          <div className="form-right-aligned-labels top-10 time-delay-container">
            <Field name="timeDelay" component={YBInputField} label="Rolling Update Delay Between Servers (secs)"/>
          </div>
        </div>
      );
    }
    return (
      <YBModal visible={modalVisible} formName={"RollingUpgradeForm"}
               onHide={formCloseAction} title={title} onFormSubmit={submitAction} error={error}>
        {formBody}
      </YBModal>
    );
  }
}
