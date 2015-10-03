/*
* Copyright (c) 2013 Jonathan Perkin <jonathan@perkin.org.uk>
* Copyright (c) 2015 Elias Karakoulakis <elias.karakoulakis@gmail.com>
* 
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
* WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
* ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
* WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
* ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
* OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include "openzwave.hpp"
#include <algorithm>

using namespace v8;
using namespace node;

namespace OZW {

	//
	uv_async_t 		async;

	// 
	Nan::Callback *emit_cb;
	
	// Message passing queue between OpenZWave callback and v8 async handler.
	mutex 		            zqueue_mutex;
	std::queue<NotifInfo *> zqueue;

	// Node state.
	mutex 		          znodes_mutex;
	std::list<NodeInfo *> znodes;

	mutex zscenes_mutex;
	std::list<SceneInfo *> zscenes;

	uint32_t homeid;
	CommandMap* ctrlCmdNames;
	std::string modulepath;
	
	// ===================================================================
	NAN_METHOD(OZW::OZW::New)
	// ===================================================================
	{
		Nan::HandleScope scope;

		assert(info.IsConstructCall());
		OZW* self = new OZW();
		self->Wrap(info.This());

		// scan for OpenZWave options.xml in the nodeJS module's '/config' subdirectory
		modulepath.append("/../../config");
		OpenZWave::Options::Create(modulepath, "", "");
		OpenZWave::Options* ozwopt = OpenZWave::Options::Get();
		
		// Options are global for all drivers and can only be set once.
		if (info.Length() > 0) {
			Local < Object > opts     = info[0]->ToObject();
			Local<Array> property_names = Nan::GetOwnPropertyNames(opts).ToLocalChecked();		
			for (unsigned int i = 0; i < property_names->Length(); ++i) {
				Local<Value> key     = property_names->Get(i);
				std::string  keyname = *v8::String::Utf8Value(key);
				Local<Value> argval  = Nan::Get(opts, key).ToLocalChecked();
				switch(ozwopt->GetOptionType(keyname)) {
				case OpenZWave::Options::OptionType_Invalid:
					std::cerr << "openzwave.cc: invalid OpenZWave option: " << keyname << "\n";
					break;
				case OpenZWave::Options::OptionType_Bool:
					ozwopt->AddOptionBool  (keyname, argval->ToBoolean()->Value());
					break;
				case OpenZWave::Options::OptionType_Int:
					ozwopt->AddOptionInt   (keyname, argval->ToInteger()->Value());
					break;
				case OpenZWave::Options::OptionType_String:
					std::string optionval = *v8::String::Utf8Value(argval);
					ozwopt->AddOptionString(keyname, optionval, true);
					break;
				}
			}
		}
		ozwopt->Lock();
		//
		// ee = new EventEmitter
		Local < Object > EventEmitter = Nan::Get( info.This(),
			Nan::New<String>("EventEmitter").ToLocalChecked()
		).ToLocalChecked().As<Object>();
		Local<Object> ee =  Nan::CallAsConstructor(EventEmitter, 0, 0).ToLocalChecked().As<Object>();
		Local<Array>  ee_props = Nan::GetPropertyNames(ee).ToLocalChecked();		
		for (unsigned int i = 0; i < ee_props->Length(); ++i) {
			Local<Value> key = ee_props->Get(i);
			Local<Value> val = Nan::Get(ee, key).ToLocalChecked();
			if (val->IsFunction()) {
				Local<Function> eefunc = val.As<Function>();
				std::cout << "copying : " << *v8::String::Utf8Value(key) << "\n";
				Nan::Set(info.This(), key, eefunc);
			}
		}
		// t->PrototypeTemplate()->Set("emit",
		
		info.GetReturnValue().Set(info.This());
	}

	// ===================================================================
	extern "C" void init(Handle<Object> target, Handle<Object> module) {
  
		Nan::HandleScope scope;
		
		std::string modulefilename = std::string(*v8::String::Utf8Value( 
			Nan::Get(module, 
				Nan::New("filename").ToLocalChecked()
			).ToLocalChecked()
		));
		std::size_t found = modulefilename.find_last_of("/\\");
		if (found > 0) {
			//std::cout << " path: " << modulefilename.substr(0,found) << '\n';
			//std::cout << " file: " << modulefilename.substr(found+1) << '\n';
			modulepath.assign(modulefilename.substr(0,found));
		}
		
		// require
   		Local<Function> require = Nan::Get( module,
			Nan::New<String>("require").ToLocalChecked()
		).ToLocalChecked().As<Function>();
        
        // require('events')
		Local<Value> requireArgs[1] = {Nan::New<String>("events").ToLocalChecked()};
		Local<Object> eventsModule  =  Nan::CallAsConstructor(
			require, 1, requireArgs
		).ToLocalChecked().As<Object>();
		
		// require('events').EventEmitter
		Local < Object > EventEmitter = Nan::Get( eventsModule,
			Nan::New<String>("EventEmitter").ToLocalChecked()
		).ToLocalChecked().As<Object>();
		assert(EventEmitter->IsFunction());
		
		// create new v8::FunctionTemplate and encapsulate the EventEmitter function
		Local < FunctionTemplate > t = Nan::New<FunctionTemplate>(OZW::New);
		t->InstanceTemplate()->SetInternalFieldCount(1);
		Nan::SetPrototypeTemplate(t, "EventEmitter", EventEmitter);
		
		// openzwave-config.cc
		Nan::SetPrototypeMethod(t, "setConfigParam", OZW::SetConfigParam);
		Nan::SetPrototypeMethod(t, "requestConfigParam", OZW::RequestConfigParam);
		Nan::SetPrototypeMethod(t, "requestAllConfigParams", OZW::RequestAllConfigParams);
		// openzwave-controller.cc
		Nan::SetPrototypeMethod(t, "hardReset", OZW::HardReset);
		Nan::SetPrototypeMethod(t, "softReset", OZW::SoftReset);
		Nan::SetPrototypeMethod(t, "beginControllerCommand", OZW::BeginControllerCommand); // ** new
		Nan::SetPrototypeMethod(t, "cancelControllerCommand", OZW::CancelControllerCommand); // ** new
		Nan::SetPrototypeMethod(t, "getControllerNodeId", OZW::GetControllerNodeId); // ** new
		Nan::SetPrototypeMethod(t, "getSUCNodeId", OZW::GetSUCNodeId); // ** new
		Nan::SetPrototypeMethod(t, "isPrimaryController", OZW::IsPrimaryController); // ** new
		Nan::SetPrototypeMethod(t, "isStaticUpdateController", OZW::IsStaticUpdateController); // ** new
		Nan::SetPrototypeMethod(t, "isBridgeController", OZW::IsBridgeController); // ** new
		Nan::SetPrototypeMethod(t, "getLibraryVersion", OZW::GetLibraryVersion); // ** new
		Nan::SetPrototypeMethod(t, "getLibraryTypeName", OZW::GetLibraryTypeName); // ** new
		Nan::SetPrototypeMethod(t, "getSendQueueCount", OZW::GetSendQueueCount);	// ** new
		// openzwave-driver.cc
		Nan::SetPrototypeMethod(t, "connect", OZW::Connect);
		Nan::SetPrototypeMethod(t, "disconnect", OZW::Disconnect);
		// openzwave-groups.cc
		Nan::SetPrototypeMethod(t, "getNumGroups", OZW::GetNumGroups);
		Nan::SetPrototypeMethod(t, "getAssociations", OZW::GetAssociations);
		Nan::SetPrototypeMethod(t, "getMaxAssociations", OZW::GetMaxAssociations);
		Nan::SetPrototypeMethod(t, "getGroupLabel", OZW::GetGroupLabel);
		Nan::SetPrototypeMethod(t, "addAssociation", OZW::AddAssociation);
		Nan::SetPrototypeMethod(t, "removeAssociation", OZW::RemoveAssociation);
		// openzwave-network.cc
		Nan::SetPrototypeMethod(t, "testNetworkNode", OZW::TestNetworkNode);
		Nan::SetPrototypeMethod(t, "testNetwork", OZW::TestNetwork);
		Nan::SetPrototypeMethod(t, "healNetworkNode", OZW::HealNetworkNode);
		Nan::SetPrototypeMethod(t, "healNetwork", OZW::HealNetwork);
		// openzwave-nodes.cc
		Nan::SetPrototypeMethod(t, "setLocation", OZW::SetLocation);
		Nan::SetPrototypeMethod(t, "setName", OZW::SetName);
		Nan::SetPrototypeMethod(t, "setNodeOn", OZW::SetNodeOn);
		Nan::SetPrototypeMethod(t, "setNodeOff", OZW::SetNodeOff);
		Nan::SetPrototypeMethod(t, "switchAllOn", OZW::SwitchAllOn);
		Nan::SetPrototypeMethod(t, "switchAllOff", OZW::SwitchAllOff);
		Nan::SetPrototypeMethod(t, "getNodeNeighbors", OZW::GetNodeNeighbors);
		Nan::SetPrototypeMethod(t, "refreshNodeInfo", OZW::RefreshNodeInfo); // ** new
		Nan::SetPrototypeMethod(t, "getNodeManufacturerName", OZW::GetNodeManufacturerName); // ** new
		Nan::SetPrototypeMethod(t, "requestNodeState", OZW::RequestNodeState); // ** new
		Nan::SetPrototypeMethod(t, "requestNodeDynamic", OZW::RequestNodeDynamic); // ** new
		Nan::SetPrototypeMethod(t, "isNodeListeningDevice", OZW::IsNodeListeningDevice); // ** new
		Nan::SetPrototypeMethod(t, "isNodeFrequentListeningDevice", OZW::IsNodeFrequentListeningDevice); // ** new
		Nan::SetPrototypeMethod(t, "isNodeBeamingDevice", OZW::IsNodeBeamingDevice); // ** new
		Nan::SetPrototypeMethod(t, "isNodeRoutingDevice", OZW::IsNodeRoutingDevice); // ** new
		Nan::SetPrototypeMethod(t, "isNodeSecurityDevice", OZW::IsNodeSecurityDevice); // ** new
		Nan::SetPrototypeMethod(t, "getNodeMaxBaudRate", OZW::GetNodeMaxBaudRate); // ** new
		Nan::SetPrototypeMethod(t, "getNodeVersion", OZW::GetNodeVersion); // ** new
		Nan::SetPrototypeMethod(t, "getNodeSecurity", OZW::GetNodeSecurity); // ** new
		Nan::SetPrototypeMethod(t, "getNodeBasic", OZW::GetNodeBasic); // ** new
		Nan::SetPrototypeMethod(t, "getNodeGeneric", OZW::GetNodeGeneric); // ** new
		Nan::SetPrototypeMethod(t, "getNodeSpecific", OZW::GetNodeSpecific); // ** new
		Nan::SetPrototypeMethod(t, "getNodeType", OZW::GetNodeType); // ** new
		Nan::SetPrototypeMethod(t, "getNodeProductName", OZW::GetNodeProductName); // ** new
		Nan::SetPrototypeMethod(t, "getNodeName", OZW::GetNodeName); // ** new
		Nan::SetPrototypeMethod(t, "getNodeLocation", OZW::GetNodeLocation); // ** new
		Nan::SetPrototypeMethod(t, "getNodeManufacturerId", OZW::GetNodeManufacturerId); // ** new
		Nan::SetPrototypeMethod(t, "getNodeProductType", OZW::GetNodeProductType); // ** new
		Nan::SetPrototypeMethod(t, "getNodeProductId", OZW::GetNodeProductId); // ** new
		Nan::SetPrototypeMethod(t, "setNodeManufacturerName", OZW::SetNodeManufacturerName); // ** new
		Nan::SetPrototypeMethod(t, "setNodeProductName", OZW::SetNodeProductName); // ** new
		// openzwave-values.cc
		Nan::SetPrototypeMethod(t, "setValue", OZW::SetValue);
		// openzwave-polling.cc
		Nan::SetPrototypeMethod(t, "enablePoll", OZW::EnablePoll);
		Nan::SetPrototypeMethod(t, "disablePoll", OZW::DisablePoll);
		Nan::SetPrototypeMethod(t, "isPolled",  OZW::IsPolled); // ** new
		Nan::SetPrototypeMethod(t, "getPollInterval",  OZW::GetPollInterval); // ** new
		Nan::SetPrototypeMethod(t, "setPollInterval",  OZW::SetPollInterval); // ** new
		Nan::SetPrototypeMethod(t, "getPollIntensity",  OZW::GetPollIntensity); // ** new
		Nan::SetPrototypeMethod(t, "setPollIntensity",  OZW::SetPollIntensity); // ** new
		// openzwave-scenes.cc
		Nan::SetPrototypeMethod(t, "createScene", OZW::CreateScene);
		Nan::SetPrototypeMethod(t, "removeScene", OZW::RemoveScene);
		Nan::SetPrototypeMethod(t, "getScenes", OZW::GetScenes);
		Nan::SetPrototypeMethod(t, "addSceneValue", OZW::AddSceneValue);
		Nan::SetPrototypeMethod(t, "removeSceneValue", OZW::RemoveSceneValue);
		Nan::SetPrototypeMethod(t, "sceneGetValues", OZW::SceneGetValues);
		Nan::SetPrototypeMethod(t, "activateScene", OZW::ActivateScene);
		// 
		Nan::Set(target, Nan::New<String>("Emitter").ToLocalChecked(), t->GetFunction());
		
		/* for BeginControllerCommand
		 * http://openzwave.com/dev/classOpenZWave_1_1Manager.html#aa11faf40f19f0cda202d2353a60dbf7b
		 */ 
		ctrlCmdNames = new CommandMap();
		// (*ctrlCmdNames)["None"] 					= OpenZWave::Driver::ControllerCommand_None;
		(*ctrlCmdNames)["AddDevice"]				= OpenZWave::Driver::ControllerCommand_AddDevice;
		(*ctrlCmdNames)["CreateNewPrimary"] 		= OpenZWave::Driver::ControllerCommand_CreateNewPrimary;
		(*ctrlCmdNames)["ReceiveConfiguration"] 	= OpenZWave::Driver::ControllerCommand_ReceiveConfiguration;
		(*ctrlCmdNames)["RemoveDevice"]  			= OpenZWave::Driver::ControllerCommand_RemoveDevice;
		(*ctrlCmdNames)["RemoveFailedNode"]			= OpenZWave::Driver::ControllerCommand_RemoveFailedNode;
		(*ctrlCmdNames)["HasNodeFailed"]			= OpenZWave::Driver::ControllerCommand_HasNodeFailed;
		(*ctrlCmdNames)["ReplaceFailedNode"]		= OpenZWave::Driver::ControllerCommand_ReplaceFailedNode;
		(*ctrlCmdNames)["TransferPrimaryRole"]		= OpenZWave::Driver::ControllerCommand_TransferPrimaryRole;
		(*ctrlCmdNames)["RequestNetworkUpdate"]		= OpenZWave::Driver::ControllerCommand_RequestNetworkUpdate;
		(*ctrlCmdNames)["RequestNodeNeighborUpdate"]= OpenZWave::Driver::ControllerCommand_RequestNodeNeighborUpdate;
		(*ctrlCmdNames)["AssignReturnRoute"] 		= OpenZWave::Driver::ControllerCommand_AssignReturnRoute;
		(*ctrlCmdNames)["DeleteAllReturnRoutes"]	= OpenZWave::Driver::ControllerCommand_DeleteAllReturnRoutes;
		(*ctrlCmdNames)["SendNodeInformation"] 		= OpenZWave::Driver::ControllerCommand_SendNodeInformation;
		(*ctrlCmdNames)["ReplicationSend"] 			= OpenZWave::Driver::ControllerCommand_ReplicationSend;
		(*ctrlCmdNames)["CreateButton"]				= OpenZWave::Driver::ControllerCommand_CreateButton;
		(*ctrlCmdNames)["DeleteButton"]				= OpenZWave::Driver::ControllerCommand_DeleteButton;
	}
}

NODE_MODULE(openzwave_shared, OZW::init)
