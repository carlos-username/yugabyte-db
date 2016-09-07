// Copyright (c) YugaByte, Inc.

import { combineReducers } from 'redux';
import CustomerReducer from './reducer_customer';
import CloudReducer from './reducer_cloud';
import UniverseReducer from './reducer_universe';
import GraphReducer from './reducer_graph';
import TasksReducer from './reducer_tasks';

import { reducer as formReducer } from 'redux-form';

const rootReducer = combineReducers({
  customer: CustomerReducer,
  cloud: CloudReducer,
  universe: UniverseReducer,
  form: formReducer,
  graph: GraphReducer,
  tasks: TasksReducer
});

export default rootReducer;
