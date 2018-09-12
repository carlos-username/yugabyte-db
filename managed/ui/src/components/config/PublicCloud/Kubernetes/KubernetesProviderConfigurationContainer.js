// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { KubernetesProviderConfiguration } from '../../../config';

import { deleteProvider, deleteProviderFailure,
  deleteProviderSuccess, fetchCloudMetadata } from 'actions/cloud';


const mapDispatchToProps = (dispatch) => {
  return {
    deleteProviderConfig: (providerUUID) => {
      dispatch(deleteProvider(providerUUID)).then((response) => {
        if (response.payload.status !== 200) {
          dispatch(deleteProviderFailure(response.payload));
        } else {
          dispatch(deleteProviderSuccess(response.payload));
          dispatch(fetchCloudMetadata());
        }
      });
    }
  };
};

const mapStateToProps = (state) => {
  return {
    providers: state.cloud.providers,
    regions: state.cloud.supportedRegionList
  };
};

export default connect(mapStateToProps, mapDispatchToProps)(KubernetesProviderConfiguration);
