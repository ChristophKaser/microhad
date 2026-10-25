// Generated typed/public-JSON boundary conversion helpers.
namespace MatterProto {
bool fromValue(const Proto::Value&,MatterStatusRequest*);
Proto::Value toValue(const MatterStatusRequest&);
bool fromValue(const Proto::Value&,ThreadStatusRequest*);
Proto::Value toValue(const ThreadStatusRequest&);
bool fromValue(const Proto::Value&,CommissioningStartRequest*);
Proto::Value toValue(const CommissioningStartRequest&);
bool fromValue(const Proto::Value&,CommissioningStatusRequest*);
Proto::Value toValue(const CommissioningStatusRequest&);
bool fromValue(const Proto::Value&,CommissioningCancelRequest*);
Proto::Value toValue(const CommissioningCancelRequest&);
bool fromValue(const Proto::Value&,BleAttachRequest*);
Proto::Value toValue(const BleAttachRequest&);
bool fromValue(const Proto::Value&,BleIndicationRequest*);
Proto::Value toValue(const BleIndicationRequest&);
bool fromValue(const Proto::Value&,BleWriteCompleteRequest*);
Proto::Value toValue(const BleWriteCompleteRequest&);
bool fromValue(const Proto::Value&,ThreadControlRequest*);
Proto::Value toValue(const ThreadControlRequest&);
bool fromValue(const Proto::Value&,NullableUint*);
Proto::Value toValue(const NullableUint&);
bool fromValue(const Proto::Value&,BindingMember*);
Proto::Value toValue(const BindingMember&);
bool fromValue(const Proto::Value&,BindingMemberList*);
Proto::Value toValue(const BindingMemberList&);
bool fromValue(const Proto::Value&,BindingTarget*);
Proto::Value toValue(const BindingTarget&);
bool fromValue(const Proto::Value&,BindingTargetList*);
Proto::Value toValue(const BindingTargetList&);
bool fromValue(const Proto::Value&,DeviceControlRequest*);
Proto::Value toValue(const DeviceControlRequest&);
bool fromValue(const Proto::Value&,DeviceCommandRequest*);
Proto::Value toValue(const DeviceCommandRequest&);
bool fromValue(const Proto::Value&,CommissioningSession*);
Proto::Value toValue(const CommissioningSession&);
bool fromValue(const Proto::Value&,BleAttachResult*);
Proto::Value toValue(const BleAttachResult&);
bool fromValue(const Proto::Value&,BleIndicationResult*);
Proto::Value toValue(const BleIndicationResult&);
bool fromValue(const Proto::Value&,BleWriteCompleteResult*);
Proto::Value toValue(const BleWriteCompleteResult&);
bool fromValue(const Proto::Value&,ThreadStatus*);
Proto::Value toValue(const ThreadStatus&);
bool fromValue(const Proto::Value&,NullableText*);
Proto::Value toValue(const NullableText&);
bool fromValue(const Proto::Value&,UInt32List*);
Proto::Value toValue(const UInt32List&);
bool fromValue(const Proto::Value&,NullableInt*);
Proto::Value toValue(const NullableInt&);
bool fromValue(const Proto::Value&,MatterDevice*);
Proto::Value toValue(const MatterDevice&);
bool fromValue(const Proto::Value&,MatterDeviceList*);
Proto::Value toValue(const MatterDeviceList&);
bool fromValue(const Proto::Value&,DeviceSnapshotPage*);
Proto::Value toValue(const DeviceSnapshotPage&);
bool fromValue(const Proto::Value&,MeasurementValue*);
Proto::Value toValue(const MeasurementValue&);
bool fromValue(const Proto::Value&,ThermostatTransition*);
Proto::Value toValue(const ThermostatTransition&);
bool fromValue(const Proto::Value&,ThermostatTransitionList*);
Proto::Value toValue(const ThermostatTransitionList&);
bool fromValue(const Proto::Value&,Measurement*);
Proto::Value toValue(const Measurement&);
bool fromValue(const Proto::Value&,ThermostatCapabilityList*);
Proto::Value toValue(const ThermostatCapabilityList&);
bool fromValue(const Proto::Value&,ReadingValue*);
Proto::Value toValue(const ReadingValue&);
bool fromValue(const Proto::Value&,AttributeValue*);
Proto::Value toValue(const AttributeValue&);
bool fromValue(const Proto::Value&,AttributeReading*);
Proto::Value toValue(const AttributeReading&);
bool fromValue(const Proto::Value&,DeviceOperationJob*);
Proto::Value toValue(const DeviceOperationJob&);
bool fromValue(const Proto::Value&,Subscription*);
Proto::Value toValue(const Subscription&);
bool fromValue(const Proto::Value&,SubscriptionList*);
Proto::Value toValue(const SubscriptionList&);
bool fromValue(const Proto::Value&,SubscriptionHealth*);
Proto::Value toValue(const SubscriptionHealth&);
bool fromValue(const Proto::Value&,OperationResult*);
Proto::Value toValue(const OperationResult&);
bool fromValue(const Proto::Value&,OperationResponse*);
Proto::Value toValue(const OperationResponse&);
bool fromValue(const Proto::Value&,BleWriteEvent*);
Proto::Value toValue(const BleWriteEvent&);
bool fromValue(const Proto::Value&,ButtonEvent*);
Proto::Value toValue(const ButtonEvent&);
bool fromValue(const Proto::Value&,InventoryRecord*);
Proto::Value toValue(const InventoryRecord&);
bool fromValue(const Proto::Value&,InventoryNode*);
Proto::Value toValue(const InventoryNode&);
bool fromValue(const Proto::Value&,InventoryNodeList*);
Proto::Value toValue(const InventoryNodeList&);
bool fromValue(const Proto::Value&,InventoryNodeManifest*);
Proto::Value toValue(const InventoryNodeManifest&);
bool fromValue(const Proto::Value&,EventPayload*);
Proto::Value toValue(const EventPayload&);
bool fromValue(const Proto::Value&,EventNotification*);
Proto::Value toValue(const EventNotification&);
bool fromValue(const Proto::Value&,MatterStatus*);
Proto::Value toValue(const MatterStatus&);
bool fromValue(const Proto::Value&,ThermostatSchedule*);
Proto::Value toValue(const ThermostatSchedule&);
bool fromValue(const Proto::Value&,MessageBody*);
Proto::Value toValue(const MessageBody&);
bool fromValue(const Proto::Value&,IpcEnvelope*);
Proto::Value toValue(const IpcEnvelope&);
bool fromValue(const Proto::Value& v,MatterStatusRequest* out){
    if(v.type!=Proto::Value::Map)return false;
    MatterStatusRequest result;
    *out=result;return true;
}
Proto::Value toValue(const MatterStatusRequest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    return out;
}
bool fromValue(const Proto::Value& v,ThreadStatusRequest* out){
    if(v.type!=Proto::Value::Map)return false;
    ThreadStatusRequest result;
    *out=result;return true;
}
Proto::Value toValue(const ThreadStatusRequest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    return out;
}
bool fromValue(const Proto::Value& v,CommissioningStartRequest* out){
    if(v.type!=Proto::Value::Map)return false;
    CommissioningStartRequest result;
    if(const Proto::Value* p=Proto::field(v,"schema")){
        result.schema.present=true;if(!getUInt32(*p,&result.schema.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"discriminator")){
        result.discriminator.present=true;if(!getUInt32(*p,&result.discriminator.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"pin")){
        result.pin.present=true;if(!getUInt32(*p,&result.pin.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"timeout_seconds")){
        result.timeout_seconds.present=true;if(!getUInt32(*p,&result.timeout_seconds.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"country_code")){
        result.country_code.present=true;if(!getText(*p,&result.country_code.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"regulatory_location")){
        result.regulatory_location.present=true;if(!getText(*p,&result.regulatory_location.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const CommissioningStartRequest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.schema.present)Proto::add(&out,"schema",jsonUInt(v.schema.value));
    if(v.discriminator.present)Proto::add(&out,"discriminator",jsonUInt(v.discriminator.value));
    if(v.pin.present)Proto::add(&out,"pin",jsonUInt(v.pin.value));
    if(v.timeout_seconds.present)Proto::add(&out,"timeout_seconds",jsonUInt(v.timeout_seconds.value));
    if(v.country_code.present)Proto::add(&out,"country_code",jsonText(v.country_code.value));
    if(v.regulatory_location.present)Proto::add(&out,"regulatory_location",jsonText(v.regulatory_location.value));
    return out;
}
bool fromValue(const Proto::Value& v,CommissioningStatusRequest* out){
    if(v.type!=Proto::Value::Map)return false;
    CommissioningStatusRequest result;
    if(const Proto::Value* p=Proto::field(v,"schema")){
        result.schema.present=true;if(!getUInt32(*p,&result.schema.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"session")){
        result.session.present=true;if(!getUInt(*p,&result.session.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"subscribe")){
        result.subscribe.present=true;if(!getBool(*p,&result.subscribe.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const CommissioningStatusRequest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.schema.present)Proto::add(&out,"schema",jsonUInt(v.schema.value));
    if(v.session.present)Proto::add(&out,"session",jsonUInt(v.session.value));
    if(v.subscribe.present)Proto::add(&out,"subscribe",jsonBool(v.subscribe.value));
    return out;
}
bool fromValue(const Proto::Value& v,CommissioningCancelRequest* out){
    if(v.type!=Proto::Value::Map)return false;
    CommissioningCancelRequest result;
    if(const Proto::Value* p=Proto::field(v,"schema")){
        result.schema.present=true;if(!getUInt32(*p,&result.schema.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"session")){
        result.session.present=true;if(!getUInt(*p,&result.session.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const CommissioningCancelRequest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.schema.present)Proto::add(&out,"schema",jsonUInt(v.schema.value));
    if(v.session.present)Proto::add(&out,"session",jsonUInt(v.session.value));
    return out;
}
bool fromValue(const Proto::Value& v,BleAttachRequest* out){
    if(v.type!=Proto::Value::Map)return false;
    BleAttachRequest result;
    if(const Proto::Value* p=Proto::field(v,"schema")){
        result.schema.present=true;if(!getUInt32(*p,&result.schema.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"session")){
        result.session.present=true;if(!getUInt(*p,&result.session.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"address")){
        result.address.present=true;if(!getBytes(*p,&result.address.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"mtu")){
        result.mtu.present=true;if(!getUInt32(*p,&result.mtu.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const BleAttachRequest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.schema.present)Proto::add(&out,"schema",jsonUInt(v.schema.value));
    if(v.session.present)Proto::add(&out,"session",jsonUInt(v.session.value));
    if(v.address.present)Proto::add(&out,"address",jsonBytes(v.address.value));
    if(v.mtu.present)Proto::add(&out,"mtu",jsonUInt(v.mtu.value));
    return out;
}
bool fromValue(const Proto::Value& v,BleIndicationRequest* out){
    if(v.type!=Proto::Value::Map)return false;
    BleIndicationRequest result;
    if(const Proto::Value* p=Proto::field(v,"schema")){
        result.schema.present=true;if(!getUInt32(*p,&result.schema.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"session")){
        result.session.present=true;if(!getUInt(*p,&result.session.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"generation")){
        result.generation.present=true;if(!getUInt32(*p,&result.generation.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"sequence")){
        result.sequence.present=true;if(!getUInt32(*p,&result.sequence.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"data")){
        result.data.present=true;if(!getBytes(*p,&result.data.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const BleIndicationRequest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.schema.present)Proto::add(&out,"schema",jsonUInt(v.schema.value));
    if(v.session.present)Proto::add(&out,"session",jsonUInt(v.session.value));
    if(v.generation.present)Proto::add(&out,"generation",jsonUInt(v.generation.value));
    if(v.sequence.present)Proto::add(&out,"sequence",jsonUInt(v.sequence.value));
    if(v.data.present)Proto::add(&out,"data",jsonBytes(v.data.value));
    return out;
}
bool fromValue(const Proto::Value& v,BleWriteCompleteRequest* out){
    if(v.type!=Proto::Value::Map)return false;
    BleWriteCompleteRequest result;
    if(const Proto::Value* p=Proto::field(v,"schema")){
        result.schema.present=true;if(!getUInt32(*p,&result.schema.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"session")){
        result.session.present=true;if(!getUInt(*p,&result.session.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"generation")){
        result.generation.present=true;if(!getUInt32(*p,&result.generation.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"write")){
        result.write.present=true;if(!getUInt32(*p,&result.write.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"success")){
        result.success.present=true;if(!getBool(*p,&result.success.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const BleWriteCompleteRequest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.schema.present)Proto::add(&out,"schema",jsonUInt(v.schema.value));
    if(v.session.present)Proto::add(&out,"session",jsonUInt(v.session.value));
    if(v.generation.present)Proto::add(&out,"generation",jsonUInt(v.generation.value));
    if(v.write.present)Proto::add(&out,"write",jsonUInt(v.write.value));
    if(v.success.present)Proto::add(&out,"success",jsonBool(v.success.value));
    return out;
}
bool fromValue(const Proto::Value& v,ThreadControlRequest* out){
    if(v.type!=Proto::Value::Map)return false;
    ThreadControlRequest result;
    if(const Proto::Value* p=Proto::field(v,"schema")){
        result.schema.present=true;if(!getUInt32(*p,&result.schema.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"action")){
        result.action.present=true;if(!getText(*p,&result.action.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"subscribe")){
        result.subscribe.present=true;if(!getBool(*p,&result.subscribe.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"zigbee_channel")){
        result.zigbee_channel.present=true;if(!getUInt32(*p,&result.zigbee_channel.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"zigbee_online")){
        result.zigbee_online.present=true;if(!getBool(*p,&result.zigbee_online.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"dataset")){
        result.dataset.present=true;if(!getBytes(*p,&result.dataset.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const ThreadControlRequest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.schema.present)Proto::add(&out,"schema",jsonUInt(v.schema.value));
    if(v.action.present)Proto::add(&out,"action",jsonText(v.action.value));
    if(v.subscribe.present)Proto::add(&out,"subscribe",jsonBool(v.subscribe.value));
    if(v.zigbee_channel.present)Proto::add(&out,"zigbee_channel",jsonUInt(v.zigbee_channel.value));
    if(v.zigbee_online.present)Proto::add(&out,"zigbee_online",jsonBool(v.zigbee_online.value));
    if(v.dataset.present)Proto::add(&out,"dataset",jsonBytes(v.dataset.value));
    return out;
}
bool fromValue(const Proto::Value& v,NullableUint* out){
    if(v.type!=Proto::Value::Map)return false;
    NullableUint result;
    if(const Proto::Value* p=Proto::field(v,"null")){
        result.null_value.present=true;if(!getBool(*p,&result.null_value.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"integer")){
        result.integer.present=true;if(!getUInt(*p,&result.integer.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const NullableUint& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.null_value.present)Proto::add(&out,"null",jsonBool(v.null_value.value));
    if(v.integer.present)Proto::add(&out,"integer",jsonUInt(v.integer.value));
    return out;
}
bool fromValue(const Proto::Value& v,BindingMember* out){
    if(v.type!=Proto::Value::Map)return false;
    BindingMember result;
    if(const Proto::Value* p=Proto::field(v,"node")){
        result.node.present=true;if(!getUInt(*p,&result.node.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"endpoint")){
        result.endpoint.present=true;if(!getUInt32(*p,&result.endpoint.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const BindingMember& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.node.present)Proto::add(&out,"node",jsonUInt(v.node.value));
    if(v.endpoint.present)Proto::add(&out,"endpoint",jsonUInt(v.endpoint.value));
    return out;
}
bool fromValue(const Proto::Value& v,BindingMemberList* out){
    if(v.type!=Proto::Value::Map)return false;
    BindingMemberList result;
    if(const Proto::Value* p=Proto::field(v,"items")){
        if(p->type!=Proto::Value::Array)return false;
        result.items.resize(p->items.size());
        for(size_t i=0;i<p->items.size();++i)if(!fromValue(p->items[i],&result.items[i]))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const BindingMemberList& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(!v.items.empty()){Proto::Value a;a.type=Proto::Value::Array;for(size_t i=0;i<v.items.size();++i)a.items.push_back(toValue(v.items[i]));Proto::add(&out,"items",a);}
    return out;
}
bool fromValue(const Proto::Value& v,BindingTarget* out){
    if(v.type!=Proto::Value::Map)return false;
    BindingTarget result;
    if(const Proto::Value* p=Proto::field(v,"node")){
        if(p->type!=Proto::Value::Null){
        result.node.present=true;if(!getUInt(*p,&result.node.value))return false;
        }
    }
    if(const Proto::Value* p=Proto::field(v,"endpoint")){
        result.endpoint.present=true;if(!fromValue(*p,&result.endpoint.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"group")){
        result.group.present=true;if(!fromValue(*p,&result.group.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"members")){
        result.members.present=true;if(!fromValue(*p,&result.members.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"cluster")){
        result.cluster.present=true;if(!getUInt32(*p,&result.cluster.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const BindingTarget& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.node.present)Proto::add(&out,"node",jsonUInt(v.node.value));
    if(v.endpoint.present)Proto::add(&out,"endpoint",toValue(v.endpoint.value));
    if(v.group.present)Proto::add(&out,"group",toValue(v.group.value));
    if(v.members.present)Proto::add(&out,"members",toValue(v.members.value));
    if(v.cluster.present)Proto::add(&out,"cluster",jsonUInt(v.cluster.value));
    return out;
}
bool fromValue(const Proto::Value& v,BindingTargetList* out){
    if(v.type!=Proto::Value::Map)return false;
    BindingTargetList result;
    if(const Proto::Value* p=Proto::field(v,"items")){
        if(p->type!=Proto::Value::Array)return false;
        result.items.resize(p->items.size());
        for(size_t i=0;i<p->items.size();++i)if(!fromValue(p->items[i],&result.items[i]))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const BindingTargetList& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(!v.items.empty()){Proto::Value a;a.type=Proto::Value::Array;for(size_t i=0;i<v.items.size();++i)a.items.push_back(toValue(v.items[i]));Proto::add(&out,"items",a);}
    return out;
}
bool fromValue(const Proto::Value& v,DeviceControlRequest* out){
    if(v.type!=Proto::Value::Map)return false;
    DeviceControlRequest result;
    if(const Proto::Value* p=Proto::field(v,"schema")){
        result.schema.present=true;if(!getUInt32(*p,&result.schema.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"action")){
        result.action.present=true;if(!getText(*p,&result.action.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"cursor")){
        result.cursor.present=true;if(!getUInt32(*p,&result.cursor.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"revision")){
        result.revision.present=true;if(!getUInt(*p,&result.revision.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"node")){
        result.node.present=true;if(!getUInt(*p,&result.node.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"endpoint")){
        result.endpoint.present=true;if(!getUInt32(*p,&result.endpoint.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"cluster")){
        result.cluster.present=true;if(!getUInt32(*p,&result.cluster.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"attribute")){
        result.attribute.present=true;if(!getUInt32(*p,&result.attribute.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"job")){
        result.job.present=true;if(!getUInt(*p,&result.job.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"targets")){
        result.targets.present=true;if(!fromValue(*p,&result.targets.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"local")){
        result.local.present=true;if(!getBool(*p,&result.local.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"restore")){
        result.restore.present=true;if(!getBool(*p,&result.restore.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const DeviceControlRequest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.schema.present)Proto::add(&out,"schema",jsonUInt(v.schema.value));
    if(v.action.present)Proto::add(&out,"action",jsonText(v.action.value));
    if(v.cursor.present)Proto::add(&out,"cursor",jsonUInt(v.cursor.value));
    if(v.revision.present)Proto::add(&out,"revision",jsonUInt(v.revision.value));
    if(v.node.present)Proto::add(&out,"node",jsonUInt(v.node.value));
    if(v.endpoint.present)Proto::add(&out,"endpoint",jsonUInt(v.endpoint.value));
    if(v.cluster.present)Proto::add(&out,"cluster",jsonUInt(v.cluster.value));
    if(v.attribute.present)Proto::add(&out,"attribute",jsonUInt(v.attribute.value));
    if(v.job.present)Proto::add(&out,"job",jsonUInt(v.job.value));
    if(v.targets.present)Proto::add(&out,"targets",toValue(v.targets.value));
    if(v.local.present)Proto::add(&out,"local",jsonBool(v.local.value));
    if(v.restore.present)Proto::add(&out,"restore",jsonBool(v.restore.value));
    return out;
}
bool fromValue(const Proto::Value& v,DeviceCommandRequest* out){
    if(v.type!=Proto::Value::Map)return false;
    DeviceCommandRequest result;
    if(const Proto::Value* p=Proto::field(v,"schema")){
        result.schema.present=true;if(!getUInt32(*p,&result.schema.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"node")){
        result.node.present=true;if(!getUInt(*p,&result.node.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"endpoint")){
        result.endpoint.present=true;if(!getUInt32(*p,&result.endpoint.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"command")){
        result.command.present=true;if(!getText(*p,&result.command.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"level")){
        result.level.present=true;if(!getUInt32(*p,&result.level.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"transition_deciseconds")){
        result.transition_deciseconds.present=true;if(!getUInt32(*p,&result.transition_deciseconds.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"field")){
        result.field.present=true;if(!getText(*p,&result.field.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"value")){
        result.value.present=true;if(!getText(*p,&result.value.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"time_zone")){
        result.time_zone.present=true;if(!getText(*p,&result.time_zone.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"offset_seconds")){
        result.offset_seconds.present=true;if(!getInt(*p,&result.offset_seconds.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const DeviceCommandRequest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.schema.present)Proto::add(&out,"schema",jsonUInt(v.schema.value));
    if(v.node.present)Proto::add(&out,"node",jsonUInt(v.node.value));
    if(v.endpoint.present)Proto::add(&out,"endpoint",jsonUInt(v.endpoint.value));
    if(v.command.present)Proto::add(&out,"command",jsonText(v.command.value));
    if(v.level.present)Proto::add(&out,"level",jsonUInt(v.level.value));
    if(v.transition_deciseconds.present)Proto::add(&out,"transition_deciseconds",jsonUInt(v.transition_deciseconds.value));
    if(v.field.present)Proto::add(&out,"field",jsonText(v.field.value));
    if(v.value.present)Proto::add(&out,"value",jsonText(v.value.value));
    if(v.time_zone.present)Proto::add(&out,"time_zone",jsonText(v.time_zone.value));
    if(v.offset_seconds.present)Proto::add(&out,"offset_seconds",jsonInt(v.offset_seconds.value));
    return out;
}
bool fromValue(const Proto::Value& v,CommissioningSession* out){
    if(v.type!=Proto::Value::Map)return false;
    CommissioningSession result;
    if(const Proto::Value* p=Proto::field(v,"session")){
        result.session.present=true;if(!getUInt(*p,&result.session.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"state")){
        result.state.present=true;if(!getText(*p,&result.state.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"deadline_remaining_seconds")){
        result.deadline_remaining_seconds.present=true;if(!getUInt32(*p,&result.deadline_remaining_seconds.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"error")){
        result.error.present=true;if(!getText(*p,&result.error.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"node")){
        result.node.present=true;if(!getUInt(*p,&result.node.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const CommissioningSession& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.session.present)Proto::add(&out,"session",jsonUInt(v.session.value));
    if(v.state.present)Proto::add(&out,"state",jsonText(v.state.value));
    if(v.deadline_remaining_seconds.present)Proto::add(&out,"deadline_remaining_seconds",jsonUInt(v.deadline_remaining_seconds.value));
    if(v.error.present)Proto::add(&out,"error",jsonText(v.error.value));
    if(v.node.present)Proto::add(&out,"node",jsonUInt(v.node.value));
    return out;
}
bool fromValue(const Proto::Value& v,BleAttachResult* out){
    if(v.type!=Proto::Value::Map)return false;
    BleAttachResult result;
    if(const Proto::Value* p=Proto::field(v,"generation")){
        result.generation.present=true;if(!getUInt32(*p,&result.generation.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const BleAttachResult& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.generation.present)Proto::add(&out,"generation",jsonUInt(v.generation.value));
    return out;
}
bool fromValue(const Proto::Value& v,BleIndicationResult* out){
    if(v.type!=Proto::Value::Map)return false;
    BleIndicationResult result;
    if(const Proto::Value* p=Proto::field(v,"sequence")){
        result.sequence.present=true;if(!getUInt32(*p,&result.sequence.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const BleIndicationResult& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.sequence.present)Proto::add(&out,"sequence",jsonUInt(v.sequence.value));
    return out;
}
bool fromValue(const Proto::Value& v,BleWriteCompleteResult* out){
    if(v.type!=Proto::Value::Map)return false;
    BleWriteCompleteResult result;
    if(const Proto::Value* p=Proto::field(v,"write")){
        result.write.present=true;if(!getUInt32(*p,&result.write.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const BleWriteCompleteResult& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.write.present)Proto::add(&out,"write",jsonUInt(v.write.value));
    return out;
}
bool fromValue(const Proto::Value& v,ThreadStatus* out){
    if(v.type!=Proto::Value::Map)return false;
    ThreadStatus result;
    if(const Proto::Value* p=Proto::field(v,"enabled")){
        result.enabled.present=true;if(!getBool(*p,&result.enabled.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"available")){
        result.available.present=true;if(!getBool(*p,&result.available.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"configured")){
        result.configured.present=true;if(!getBool(*p,&result.configured.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"attached")){
        result.attached.present=true;if(!getBool(*p,&result.attached.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"state")){
        result.state.present=true;if(!getText(*p,&result.state.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"linux_ipv6_or_tun")){
        result.linux_ipv6_or_tun.present=true;if(!getBool(*p,&result.linux_ipv6_or_tun.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"zigbee_channel")){
        result.zigbee_channel.present=true;if(!getUInt32(*p,&result.zigbee_channel.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"dataset_channel")){
        result.dataset_channel.present=true;if(!getUInt32(*p,&result.dataset_channel.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const ThreadStatus& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.enabled.present)Proto::add(&out,"enabled",jsonBool(v.enabled.value));
    if(v.available.present)Proto::add(&out,"available",jsonBool(v.available.value));
    if(v.configured.present)Proto::add(&out,"configured",jsonBool(v.configured.value));
    if(v.attached.present)Proto::add(&out,"attached",jsonBool(v.attached.value));
    if(v.state.present)Proto::add(&out,"state",jsonText(v.state.value));
    if(v.linux_ipv6_or_tun.present)Proto::add(&out,"linux_ipv6_or_tun",jsonBool(v.linux_ipv6_or_tun.value));
    if(v.zigbee_channel.present)Proto::add(&out,"zigbee_channel",jsonUInt(v.zigbee_channel.value));
    if(v.dataset_channel.present)Proto::add(&out,"dataset_channel",jsonUInt(v.dataset_channel.value));
    return out;
}
bool fromValue(const Proto::Value& v,NullableText* out){
    if(v.type!=Proto::Value::Map)return false;
    NullableText result;
    if(const Proto::Value* p=Proto::field(v,"null")){
        result.null_value.present=true;if(!getBool(*p,&result.null_value.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"text")){
        result.text.present=true;if(!getText(*p,&result.text.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const NullableText& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.null_value.present)Proto::add(&out,"null",jsonBool(v.null_value.value));
    if(v.text.present)Proto::add(&out,"text",jsonText(v.text.value));
    return out;
}
bool fromValue(const Proto::Value& v,UInt32List* out){
    if(v.type!=Proto::Value::Map)return false;
    UInt32List result;
    if(const Proto::Value* p=Proto::field(v,"items")){
        if(p->type!=Proto::Value::Array)return false;
        result.items.resize(p->items.size());
        for(size_t i=0;i<p->items.size();++i)if(!getUInt32(p->items[i],&result.items[i]))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const UInt32List& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(!v.items.empty()){Proto::Value a;a.type=Proto::Value::Array;for(size_t i=0;i<v.items.size();++i)a.items.push_back(jsonUInt(v.items[i]));Proto::add(&out,"items",a);}
    return out;
}
bool fromValue(const Proto::Value& v,NullableInt* out){
    if(v.type!=Proto::Value::Map)return false;
    NullableInt result;
    if(const Proto::Value* p=Proto::field(v,"null")){
        result.null_value.present=true;if(!getBool(*p,&result.null_value.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"integer")){
        result.integer.present=true;if(!getInt(*p,&result.integer.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const NullableInt& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.null_value.present)Proto::add(&out,"null",jsonBool(v.null_value.value));
    if(v.integer.present)Proto::add(&out,"integer",jsonInt(v.integer.value));
    return out;
}
bool fromValue(const Proto::Value& v,MatterDevice* out){
    if(v.type!=Proto::Value::Map)return false;
    MatterDevice result;
    if(const Proto::Value* p=Proto::field(v,"node")){
        result.node.present=true;if(!getUInt(*p,&result.node.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"state")){
        result.state.present=true;if(!getText(*p,&result.state.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"online")){
        result.online.present=true;if(!getBool(*p,&result.online.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"id")){
        result.id.present=true;if(!fromValue(*p,&result.id.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"endpoint")){
        result.endpoint.present=true;if(!fromValue(*p,&result.endpoint.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"servers")){
        result.servers.present=true;if(!fromValue(*p,&result.servers.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"clients")){
        result.clients.present=true;if(!fromValue(*p,&result.clients.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"manufacturer")){
        result.manufacturer.present=true;if(!getText(*p,&result.manufacturer.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"model")){
        result.model.present=true;if(!getText(*p,&result.model.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"rssi_dbm")){
        result.rssi_dbm.present=true;if(!fromValue(*p,&result.rssi_dbm.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"device_types")){
        result.device_types.present=true;if(!fromValue(*p,&result.device_types.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const MatterDevice& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.node.present)Proto::add(&out,"node",jsonUInt(v.node.value));
    if(v.state.present)Proto::add(&out,"state",jsonText(v.state.value));
    if(v.online.present)Proto::add(&out,"online",jsonBool(v.online.value));
    if(v.id.present)Proto::add(&out,"id",toValue(v.id.value));
    if(v.endpoint.present)Proto::add(&out,"endpoint",toValue(v.endpoint.value));
    if(v.servers.present)Proto::add(&out,"servers",toValue(v.servers.value));
    if(v.clients.present)Proto::add(&out,"clients",toValue(v.clients.value));
    if(v.manufacturer.present)Proto::add(&out,"manufacturer",jsonText(v.manufacturer.value));
    if(v.model.present)Proto::add(&out,"model",jsonText(v.model.value));
    if(v.rssi_dbm.present)Proto::add(&out,"rssi_dbm",toValue(v.rssi_dbm.value));
    if(v.device_types.present)Proto::add(&out,"device_types",toValue(v.device_types.value));
    return out;
}
bool fromValue(const Proto::Value& v,MatterDeviceList* out){
    if(v.type!=Proto::Value::Map)return false;
    MatterDeviceList result;
    if(const Proto::Value* p=Proto::field(v,"items")){
        if(p->type!=Proto::Value::Array)return false;
        result.items.resize(p->items.size());
        for(size_t i=0;i<p->items.size();++i)if(!fromValue(p->items[i],&result.items[i]))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const MatterDeviceList& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(!v.items.empty()){Proto::Value a;a.type=Proto::Value::Array;for(size_t i=0;i<v.items.size();++i)a.items.push_back(toValue(v.items[i]));Proto::add(&out,"items",a);}
    return out;
}
bool fromValue(const Proto::Value& v,DeviceSnapshotPage* out){
    if(v.type!=Proto::Value::Map)return false;
    DeviceSnapshotPage result;
    if(const Proto::Value* p=Proto::field(v,"revision")){
        result.revision.present=true;if(!getUInt(*p,&result.revision.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"devices")){
        result.devices.present=true;if(!fromValue(*p,&result.devices.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"next_cursor")){
        result.next_cursor.present=true;if(!fromValue(*p,&result.next_cursor.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const DeviceSnapshotPage& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.revision.present)Proto::add(&out,"revision",jsonUInt(v.revision.value));
    if(v.devices.present)Proto::add(&out,"devices",toValue(v.devices.value));
    if(v.next_cursor.present)Proto::add(&out,"next_cursor",toValue(v.next_cursor.value));
    return out;
}
bool fromValue(const Proto::Value& v,MeasurementValue* out){
    if(v.type!=Proto::Value::Map)return false;
    MeasurementValue result;
    if(const Proto::Value* p=Proto::field(v,"null")){
        result.null_value.present=true;if(!getBool(*p,&result.null_value.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"number")){
        result.number.present=true;if(!getFloat(*p,&result.number.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"integer")){
        result.integer.present=true;if(!getInt(*p,&result.integer.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"text")){
        result.text.present=true;if(!getText(*p,&result.text.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const MeasurementValue& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.null_value.present)Proto::add(&out,"null",jsonBool(v.null_value.value));
    if(v.number.present)Proto::add(&out,"number",jsonFloat(v.number.value));
    if(v.integer.present)Proto::add(&out,"integer",jsonInt(v.integer.value));
    if(v.text.present)Proto::add(&out,"text",jsonText(v.text.value));
    return out;
}
bool fromValue(const Proto::Value& v,ThermostatTransition* out){
    if(v.type!=Proto::Value::Map)return false;
    ThermostatTransition result;
    if(const Proto::Value* p=Proto::field(v,"time")){
        result.time.present=true;if(!getUInt32(*p,&result.time.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"heat")){
        result.heat.present=true;if(!getFloat(*p,&result.heat.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"cool")){
        result.cool.present=true;if(!getFloat(*p,&result.cool.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const ThermostatTransition& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.time.present)Proto::add(&out,"time",jsonUInt(v.time.value));
    if(v.heat.present)Proto::add(&out,"heat",jsonFloat(v.heat.value));
    if(v.cool.present)Proto::add(&out,"cool",jsonFloat(v.cool.value));
    return out;
}
bool fromValue(const Proto::Value& v,ThermostatTransitionList* out){
    if(v.type!=Proto::Value::Map)return false;
    ThermostatTransitionList result;
    if(const Proto::Value* p=Proto::field(v,"items")){
        if(p->type!=Proto::Value::Array)return false;
        result.items.resize(p->items.size());
        for(size_t i=0;i<p->items.size();++i)if(!fromValue(p->items[i],&result.items[i]))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const ThermostatTransitionList& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(!v.items.empty()){Proto::Value a;a.type=Proto::Value::Array;for(size_t i=0;i<v.items.size();++i)a.items.push_back(toValue(v.items[i]));Proto::add(&out,"items",a);}
    return out;
}
bool fromValue(const Proto::Value& v,Measurement* out){
    if(v.type!=Proto::Value::Map)return false;
    Measurement result;
    if(const Proto::Value* p=Proto::field(v,"value")){
        result.value.present=true;if(!fromValue(*p,&result.value.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"unit")){
        result.unit.present=true;if(!getText(*p,&result.unit.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"quality")){
        result.quality.present=true;if(!getText(*p,&result.quality.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"mode")){
        result.mode.present=true;if(!getUInt32(*p,&result.mode.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"transitions")){
        result.transitions.present=true;if(!fromValue(*p,&result.transitions.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const Measurement& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.value.present)Proto::add(&out,"value",toValue(v.value.value));
    if(v.unit.present)Proto::add(&out,"unit",jsonText(v.unit.value));
    if(v.quality.present)Proto::add(&out,"quality",jsonText(v.quality.value));
    if(v.mode.present)Proto::add(&out,"mode",jsonUInt(v.mode.value));
    if(v.transitions.present)Proto::add(&out,"transitions",toValue(v.transitions.value));
    return out;
}
bool fromValue(const Proto::Value& v,ThermostatCapabilityList* out){
    if(v.type!=Proto::Value::Map)return false;
    ThermostatCapabilityList result;
    if(const Proto::Value* p=Proto::field(v,"items")){
        if(p->type!=Proto::Value::Array)return false;
        result.items.resize(p->items.size());
        for(size_t i=0;i<p->items.size();++i)if(!getUInt32(p->items[i],&result.items[i]))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const ThermostatCapabilityList& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(!v.items.empty()){Proto::Value a;a.type=Proto::Value::Array;for(size_t i=0;i<v.items.size();++i)a.items.push_back(jsonUInt(v.items[i]));Proto::add(&out,"items",a);}
    return out;
}
bool fromValue(const Proto::Value& v,ReadingValue* out){
    if(v.type!=Proto::Value::Map)return false;
    ReadingValue result;
    if(const Proto::Value* p=Proto::field(v,"null")){
        result.null_value.present=true;if(!getBool(*p,&result.null_value.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"boolean")){
        result.boolean.present=true;if(!getBool(*p,&result.boolean.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"unsigned")){
        result.unsigned_value.present=true;if(!getUInt(*p,&result.unsigned_value.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"integer")){
        result.integer.present=true;if(!getInt(*p,&result.integer.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"number")){
        result.number.present=true;if(!getFloat(*p,&result.number.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"text")){
        result.text.present=true;if(!getText(*p,&result.text.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"measurement")){
        result.measurement.present=true;if(!fromValue(*p,&result.measurement.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"list")){
        result.list.present=true;if(!fromValue(*p,&result.list.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const ReadingValue& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.null_value.present)Proto::add(&out,"null",jsonBool(v.null_value.value));
    if(v.boolean.present)Proto::add(&out,"boolean",jsonBool(v.boolean.value));
    if(v.unsigned_value.present)Proto::add(&out,"unsigned",jsonUInt(v.unsigned_value.value));
    if(v.integer.present)Proto::add(&out,"integer",jsonInt(v.integer.value));
    if(v.number.present)Proto::add(&out,"number",jsonFloat(v.number.value));
    if(v.text.present)Proto::add(&out,"text",jsonText(v.text.value));
    if(v.measurement.present)Proto::add(&out,"measurement",toValue(v.measurement.value));
    if(v.list.present)Proto::add(&out,"list",toValue(v.list.value));
    return out;
}
bool fromValue(const Proto::Value& v,AttributeValue* out){
    if(v.type!=Proto::Value::Map)return false;
    AttributeValue result;
    if(const Proto::Value* p=Proto::field(v,"type")){
        result.type.present=true;if(!getText(*p,&result.type.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"value")){
        result.value.present=true;if(!fromValue(*p,&result.value.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const AttributeValue& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.type.present)Proto::add(&out,"type",jsonText(v.type.value));
    if(v.value.present)Proto::add(&out,"value",toValue(v.value.value));
    return out;
}
bool fromValue(const Proto::Value& v,AttributeReading* out){
    if(v.type!=Proto::Value::Map)return false;
    AttributeReading result;
    if(const Proto::Value* p=Proto::field(v,"node")){
        result.node.present=true;if(!getUInt(*p,&result.node.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"endpoint")){
        result.endpoint.present=true;if(!getUInt32(*p,&result.endpoint.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"cluster")){
        result.cluster.present=true;if(!getUInt32(*p,&result.cluster.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"attribute")){
        result.attribute.present=true;if(!getUInt32(*p,&result.attribute.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"value")){
        result.value.present=true;if(!fromValue(*p,&result.value.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const AttributeReading& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.node.present)Proto::add(&out,"node",jsonUInt(v.node.value));
    if(v.endpoint.present)Proto::add(&out,"endpoint",jsonUInt(v.endpoint.value));
    if(v.cluster.present)Proto::add(&out,"cluster",jsonUInt(v.cluster.value));
    if(v.attribute.present)Proto::add(&out,"attribute",jsonUInt(v.attribute.value));
    if(v.value.present)Proto::add(&out,"value",toValue(v.value.value));
    return out;
}
bool fromValue(const Proto::Value& v,DeviceOperationJob* out){
    if(v.type!=Proto::Value::Map)return false;
    DeviceOperationJob result;
    if(const Proto::Value* p=Proto::field(v,"job")){
        result.job.present=true;if(!getUInt(*p,&result.job.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"state")){
        result.state.present=true;if(!getText(*p,&result.state.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"error")){
        result.error.present=true;if(!getText(*p,&result.error.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"result")){
        result.result.present=true;if(!fromValue(*p,&result.result.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const DeviceOperationJob& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.job.present)Proto::add(&out,"job",jsonUInt(v.job.value));
    if(v.state.present)Proto::add(&out,"state",jsonText(v.state.value));
    if(v.error.present)Proto::add(&out,"error",jsonText(v.error.value));
    if(v.result.present)Proto::add(&out,"result",toValue(v.result.value));
    return out;
}
bool fromValue(const Proto::Value& v,Subscription* out){
    if(v.type!=Proto::Value::Map)return false;
    Subscription result;
    if(const Proto::Value* p=Proto::field(v,"endpoint")){
        result.endpoint.present=true;if(!getUInt32(*p,&result.endpoint.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"cluster")){
        result.cluster.present=true;if(!getUInt32(*p,&result.cluster.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"attribute")){
        result.attribute.present=true;if(!getUInt32(*p,&result.attribute.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"id")){
        result.id.present=true;if(!fromValue(*p,&result.id.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"max_interval_seconds")){
        result.max_interval_seconds.present=true;if(!getUInt32(*p,&result.max_interval_seconds.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"last_report_age_seconds")){
        result.last_report_age_seconds.present=true;if(!fromValue(*p,&result.last_report_age_seconds.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"retry_in_seconds")){
        result.retry_in_seconds.present=true;if(!getUInt32(*p,&result.retry_in_seconds.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"failures")){
        result.failures.present=true;if(!getUInt32(*p,&result.failures.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"state")){
        result.state.present=true;if(!getText(*p,&result.state.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const Subscription& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.endpoint.present)Proto::add(&out,"endpoint",jsonUInt(v.endpoint.value));
    if(v.cluster.present)Proto::add(&out,"cluster",jsonUInt(v.cluster.value));
    if(v.attribute.present)Proto::add(&out,"attribute",jsonUInt(v.attribute.value));
    if(v.id.present)Proto::add(&out,"id",toValue(v.id.value));
    if(v.max_interval_seconds.present)Proto::add(&out,"max_interval_seconds",jsonUInt(v.max_interval_seconds.value));
    if(v.last_report_age_seconds.present)Proto::add(&out,"last_report_age_seconds",toValue(v.last_report_age_seconds.value));
    if(v.retry_in_seconds.present)Proto::add(&out,"retry_in_seconds",jsonUInt(v.retry_in_seconds.value));
    if(v.failures.present)Proto::add(&out,"failures",jsonUInt(v.failures.value));
    if(v.state.present)Proto::add(&out,"state",jsonText(v.state.value));
    return out;
}
bool fromValue(const Proto::Value& v,SubscriptionList* out){
    if(v.type!=Proto::Value::Map)return false;
    SubscriptionList result;
    if(const Proto::Value* p=Proto::field(v,"items")){
        if(p->type!=Proto::Value::Array)return false;
        result.items.resize(p->items.size());
        for(size_t i=0;i<p->items.size();++i)if(!fromValue(p->items[i],&result.items[i]))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const SubscriptionList& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(!v.items.empty()){Proto::Value a;a.type=Proto::Value::Array;for(size_t i=0;i<v.items.size();++i)a.items.push_back(toValue(v.items[i]));Proto::add(&out,"items",a);}
    return out;
}
bool fromValue(const Proto::Value& v,SubscriptionHealth* out){
    if(v.type!=Proto::Value::Map)return false;
    SubscriptionHealth result;
    if(const Proto::Value* p=Proto::field(v,"node")){
        result.node.present=true;if(!getUInt(*p,&result.node.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"restore_scheduled")){
        result.restore_scheduled.present=true;if(!getBool(*p,&result.restore_scheduled.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"intermittent")){
        result.intermittent.present=true;if(!getBool(*p,&result.intermittent.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"reachability")){
        result.reachability.present=true;if(!getText(*p,&result.reachability.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"next_cursor")){
        result.next_cursor.present=true;if(!fromValue(*p,&result.next_cursor.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"subscriptions")){
        result.subscriptions.present=true;if(!fromValue(*p,&result.subscriptions.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const SubscriptionHealth& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.node.present)Proto::add(&out,"node",jsonUInt(v.node.value));
    if(v.restore_scheduled.present)Proto::add(&out,"restore_scheduled",jsonBool(v.restore_scheduled.value));
    if(v.intermittent.present)Proto::add(&out,"intermittent",jsonBool(v.intermittent.value));
    if(v.reachability.present)Proto::add(&out,"reachability",jsonText(v.reachability.value));
    if(v.next_cursor.present)Proto::add(&out,"next_cursor",toValue(v.next_cursor.value));
    if(v.subscriptions.present)Proto::add(&out,"subscriptions",toValue(v.subscriptions.value));
    return out;
}
bool fromValue(const Proto::Value& v,OperationResult* out){
    if(v.type!=Proto::Value::Map)return false;
    OperationResult result;
    if(const Proto::Value* p=Proto::field(v,"session")){
        result.session.present=true;if(!fromValue(*p,&result.session.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"ble_attach_result")){
        result.ble_attach_result.present=true;if(!fromValue(*p,&result.ble_attach_result.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"ble_indication_result")){
        result.ble_indication_result.present=true;if(!fromValue(*p,&result.ble_indication_result.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"ble_write_complete_result")){
        result.ble_write_complete_result.present=true;if(!fromValue(*p,&result.ble_write_complete_result.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"thread")){
        result.thread.present=true;if(!fromValue(*p,&result.thread.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"page")){
        result.page.present=true;if(!fromValue(*p,&result.page.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"job")){
        result.job.present=true;if(!fromValue(*p,&result.job.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"subscription_health")){
        result.subscription_health.present=true;if(!fromValue(*p,&result.subscription_health.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const OperationResult& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.session.present)Proto::add(&out,"session",toValue(v.session.value));
    if(v.ble_attach_result.present)Proto::add(&out,"ble_attach_result",toValue(v.ble_attach_result.value));
    if(v.ble_indication_result.present)Proto::add(&out,"ble_indication_result",toValue(v.ble_indication_result.value));
    if(v.ble_write_complete_result.present)Proto::add(&out,"ble_write_complete_result",toValue(v.ble_write_complete_result.value));
    if(v.thread.present)Proto::add(&out,"thread",toValue(v.thread.value));
    if(v.page.present)Proto::add(&out,"page",toValue(v.page.value));
    if(v.job.present)Proto::add(&out,"job",toValue(v.job.value));
    if(v.subscription_health.present)Proto::add(&out,"subscription_health",toValue(v.subscription_health.value));
    return out;
}
bool fromValue(const Proto::Value& v,OperationResponse* out){
    if(v.type!=Proto::Value::Map)return false;
    OperationResponse result;
    if(const Proto::Value* p=Proto::field(v,"schema")){
        result.schema.present=true;if(!getUInt32(*p,&result.schema.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"ok")){
        result.ok.present=true;if(!getBool(*p,&result.ok.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"error")){
        result.error.present=true;if(!getText(*p,&result.error.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"result")){
        result.result.present=true;if(!fromValue(*p,&result.result.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const OperationResponse& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.schema.present)Proto::add(&out,"schema",jsonUInt(v.schema.value));
    if(v.ok.present)Proto::add(&out,"ok",jsonBool(v.ok.value));
    if(v.error.present)Proto::add(&out,"error",jsonText(v.error.value));
    if(v.result.present)Proto::add(&out,"result",toValue(v.result.value));
    return out;
}
bool fromValue(const Proto::Value& v,BleWriteEvent* out){
    if(v.type!=Proto::Value::Map)return false;
    BleWriteEvent result;
    if(const Proto::Value* p=Proto::field(v,"session")){
        result.session.present=true;if(!getUInt(*p,&result.session.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"generation")){
        result.generation.present=true;if(!getUInt32(*p,&result.generation.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"write")){
        result.write.present=true;if(!getUInt32(*p,&result.write.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"data")){
        result.data.present=true;if(!getBytes(*p,&result.data.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const BleWriteEvent& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.session.present)Proto::add(&out,"session",jsonUInt(v.session.value));
    if(v.generation.present)Proto::add(&out,"generation",jsonUInt(v.generation.value));
    if(v.write.present)Proto::add(&out,"write",jsonUInt(v.write.value));
    if(v.data.present)Proto::add(&out,"data",jsonBytes(v.data.value));
    return out;
}
bool fromValue(const Proto::Value& v,ButtonEvent* out){
    if(v.type!=Proto::Value::Map)return false;
    ButtonEvent result;
    if(const Proto::Value* p=Proto::field(v,"node")){
        result.node.present=true;if(!getUInt(*p,&result.node.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"endpoint")){
        result.endpoint.present=true;if(!getUInt32(*p,&result.endpoint.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"event")){
        result.event.present=true;if(!getUInt32(*p,&result.event.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"position")){
        if(p->type!=Proto::Value::Null){
        result.position.present=true;if(!getUInt32(*p,&result.position.value))return false;
        }
    }
    if(const Proto::Value* p=Proto::field(v,"press_count")){
        if(p->type!=Proto::Value::Null){
        result.press_count.present=true;if(!getUInt32(*p,&result.press_count.value))return false;
        }
    }
    *out=result;return true;
}
Proto::Value toValue(const ButtonEvent& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.node.present)Proto::add(&out,"node",jsonUInt(v.node.value));
    if(v.endpoint.present)Proto::add(&out,"endpoint",jsonUInt(v.endpoint.value));
    if(v.event.present)Proto::add(&out,"event",jsonUInt(v.event.value));
    if(v.position.present)Proto::add(&out,"position",jsonUInt(v.position.value));
    if(v.press_count.present)Proto::add(&out,"press_count",jsonUInt(v.press_count.value));
    return out;
}
bool fromValue(const Proto::Value& v,InventoryRecord* out){
    if(v.type!=Proto::Value::Map)return false;
    InventoryRecord result;
    if(const Proto::Value* p=Proto::field(v,"kind")){
        result.kind.present=true;if(!getText(*p,&result.kind.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"device")){
        result.device.present=true;if(!fromValue(*p,&result.device.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"endpoint_count")){
        result.endpoint_count.present=true;if(!getUInt32(*p,&result.endpoint_count.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const InventoryRecord& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.kind.present)Proto::add(&out,"kind",jsonText(v.kind.value));
    if(v.device.present)Proto::add(&out,"device",toValue(v.device.value));
    if(v.endpoint_count.present)Proto::add(&out,"endpoint_count",jsonUInt(v.endpoint_count.value));
    return out;
}
bool fromValue(const Proto::Value& v,InventoryNode* out){
    if(v.type!=Proto::Value::Map)return false;
    InventoryNode result;
    if(const Proto::Value* p=Proto::field(v,"node")){
        result.node.present=true;if(!getUInt(*p,&result.node.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const InventoryNode& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.node.present)Proto::add(&out,"node",jsonUInt(v.node.value));
    return out;
}
bool fromValue(const Proto::Value& v,InventoryNodeList* out){
    if(v.type!=Proto::Value::Map)return false;
    InventoryNodeList result;
    if(const Proto::Value* p=Proto::field(v,"items")){
        if(p->type!=Proto::Value::Array)return false;
        result.items.resize(p->items.size());
        for(size_t i=0;i<p->items.size();++i)if(!fromValue(p->items[i],&result.items[i]))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const InventoryNodeList& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(!v.items.empty()){Proto::Value a;a.type=Proto::Value::Array;for(size_t i=0;i<v.items.size();++i)a.items.push_back(toValue(v.items[i]));Proto::add(&out,"items",a);}
    return out;
}
bool fromValue(const Proto::Value& v,InventoryNodeManifest* out){
    if(v.type!=Proto::Value::Map)return false;
    InventoryNodeManifest result;
    if(const Proto::Value* p=Proto::field(v,"nodes")){
        result.nodes.present=true;if(!fromValue(*p,&result.nodes.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const InventoryNodeManifest& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.nodes.present)Proto::add(&out,"nodes",toValue(v.nodes.value));
    return out;
}
bool fromValue(const Proto::Value& v,EventPayload* out){
    if(v.type!=Proto::Value::Map)return false;
    EventPayload result;
    if(const Proto::Value* p=Proto::field(v,"thread")){
        result.thread.present=true;if(!fromValue(*p,&result.thread.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"session")){
        result.session.present=true;if(!fromValue(*p,&result.session.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"ble_write")){
        result.ble_write.present=true;if(!fromValue(*p,&result.ble_write.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"reading")){
        result.reading.present=true;if(!fromValue(*p,&result.reading.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"button")){
        result.button.present=true;if(!fromValue(*p,&result.button.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"inventory")){
        result.inventory.present=true;if(!fromValue(*p,&result.inventory.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"inventory_nodes")){
        result.inventory_nodes.present=true;if(!fromValue(*p,&result.inventory_nodes.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const EventPayload& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.thread.present)Proto::add(&out,"thread",toValue(v.thread.value));
    if(v.session.present)Proto::add(&out,"session",toValue(v.session.value));
    if(v.ble_write.present)Proto::add(&out,"ble_write",toValue(v.ble_write.value));
    if(v.reading.present)Proto::add(&out,"reading",toValue(v.reading.value));
    if(v.button.present)Proto::add(&out,"button",toValue(v.button.value));
    if(v.inventory.present)Proto::add(&out,"inventory",toValue(v.inventory.value));
    if(v.inventory_nodes.present)Proto::add(&out,"inventory_nodes",toValue(v.inventory_nodes.value));
    return out;
}
bool fromValue(const Proto::Value& v,EventNotification* out){
    if(v.type!=Proto::Value::Map)return false;
    EventNotification result;
    if(const Proto::Value* p=Proto::field(v,"schema")){
        result.schema.present=true;if(!getUInt32(*p,&result.schema.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"event")){
        result.event.present=true;if(!getText(*p,&result.event.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"data")){
        result.data.present=true;if(!fromValue(*p,&result.data.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const EventNotification& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.schema.present)Proto::add(&out,"schema",jsonUInt(v.schema.value));
    if(v.event.present)Proto::add(&out,"event",jsonText(v.event.value));
    if(v.data.present)Proto::add(&out,"data",toValue(v.data.value));
    return out;
}
bool fromValue(const Proto::Value& v,MatterStatus* out){
    if(v.type!=Proto::Value::Map)return false;
    MatterStatus result;
    if(const Proto::Value* p=Proto::field(v,"enabled")){
        result.enabled.present=true;if(!getBool(*p,&result.enabled.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"available")){
        result.available.present=true;if(!getBool(*p,&result.available.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"controller_initialized")){
        result.controller_initialized.present=true;if(!getBool(*p,&result.controller_initialized.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"fabric_ready")){
        result.fabric_ready.present=true;if(!getBool(*p,&result.fabric_ready.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"state")){
        result.state.present=true;if(!getText(*p,&result.state.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"commissioning_session")){
        result.commissioning_session.present=true;if(!getUInt(*p,&result.commissioning_session.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const MatterStatus& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.enabled.present)Proto::add(&out,"enabled",jsonBool(v.enabled.value));
    if(v.available.present)Proto::add(&out,"available",jsonBool(v.available.value));
    if(v.controller_initialized.present)Proto::add(&out,"controller_initialized",jsonBool(v.controller_initialized.value));
    if(v.fabric_ready.present)Proto::add(&out,"fabric_ready",jsonBool(v.fabric_ready.value));
    if(v.state.present)Proto::add(&out,"state",jsonText(v.state.value));
    if(v.commissioning_session.present)Proto::add(&out,"commissioning_session",jsonUInt(v.commissioning_session.value));
    return out;
}
bool fromValue(const Proto::Value& v,ThermostatSchedule* out){
    if(v.type!=Proto::Value::Map)return false;
    ThermostatSchedule result;
    if(const Proto::Value* p=Proto::field(v,"mode")){
        result.mode.present=true;if(!getUInt32(*p,&result.mode.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"transitions")){
        result.transitions.present=true;if(!fromValue(*p,&result.transitions.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const ThermostatSchedule& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.mode.present)Proto::add(&out,"mode",jsonUInt(v.mode.value));
    if(v.transitions.present)Proto::add(&out,"transitions",toValue(v.transitions.value));
    return out;
}
bool fromValue(const Proto::Value& v,MessageBody* out){
    if(v.type!=Proto::Value::Map)return false;
    MessageBody result;
    if(const Proto::Value* p=Proto::field(v,"matter_status_request")){
        result.matter_status_request.present=true;if(!fromValue(*p,&result.matter_status_request.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"thread_status_request")){
        result.thread_status_request.present=true;if(!fromValue(*p,&result.thread_status_request.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"commissioning_start_request")){
        result.commissioning_start_request.present=true;if(!fromValue(*p,&result.commissioning_start_request.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"commissioning_status_request")){
        result.commissioning_status_request.present=true;if(!fromValue(*p,&result.commissioning_status_request.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"commissioning_cancel_request")){
        result.commissioning_cancel_request.present=true;if(!fromValue(*p,&result.commissioning_cancel_request.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"ble_attach_request")){
        result.ble_attach_request.present=true;if(!fromValue(*p,&result.ble_attach_request.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"ble_indication_request")){
        result.ble_indication_request.present=true;if(!fromValue(*p,&result.ble_indication_request.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"ble_write_complete_request")){
        result.ble_write_complete_request.present=true;if(!fromValue(*p,&result.ble_write_complete_request.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"thread_control_request")){
        result.thread_control_request.present=true;if(!fromValue(*p,&result.thread_control_request.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"device_control_request")){
        result.device_control_request.present=true;if(!fromValue(*p,&result.device_control_request.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"device_command_request")){
        result.device_command_request.present=true;if(!fromValue(*p,&result.device_command_request.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"matter")){
        result.matter.present=true;if(!fromValue(*p,&result.matter.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"thread")){
        result.thread.present=true;if(!fromValue(*p,&result.thread.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"reply")){
        result.reply.present=true;if(!fromValue(*p,&result.reply.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"event")){
        result.event.present=true;if(!fromValue(*p,&result.event.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const MessageBody& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.matter_status_request.present)Proto::add(&out,"matter_status_request",toValue(v.matter_status_request.value));
    if(v.thread_status_request.present)Proto::add(&out,"thread_status_request",toValue(v.thread_status_request.value));
    if(v.commissioning_start_request.present)Proto::add(&out,"commissioning_start_request",toValue(v.commissioning_start_request.value));
    if(v.commissioning_status_request.present)Proto::add(&out,"commissioning_status_request",toValue(v.commissioning_status_request.value));
    if(v.commissioning_cancel_request.present)Proto::add(&out,"commissioning_cancel_request",toValue(v.commissioning_cancel_request.value));
    if(v.ble_attach_request.present)Proto::add(&out,"ble_attach_request",toValue(v.ble_attach_request.value));
    if(v.ble_indication_request.present)Proto::add(&out,"ble_indication_request",toValue(v.ble_indication_request.value));
    if(v.ble_write_complete_request.present)Proto::add(&out,"ble_write_complete_request",toValue(v.ble_write_complete_request.value));
    if(v.thread_control_request.present)Proto::add(&out,"thread_control_request",toValue(v.thread_control_request.value));
    if(v.device_control_request.present)Proto::add(&out,"device_control_request",toValue(v.device_control_request.value));
    if(v.device_command_request.present)Proto::add(&out,"device_command_request",toValue(v.device_command_request.value));
    if(v.matter.present)Proto::add(&out,"matter",toValue(v.matter.value));
    if(v.thread.present)Proto::add(&out,"thread",toValue(v.thread.value));
    if(v.reply.present)Proto::add(&out,"reply",toValue(v.reply.value));
    if(v.event.present)Proto::add(&out,"event",toValue(v.event.value));
    return out;
}
bool fromValue(const Proto::Value& v,IpcEnvelope* out){
    if(v.type!=Proto::Value::Map)return false;
    IpcEnvelope result;
    if(const Proto::Value* p=Proto::field(v,"version")){
        result.version.present=true;if(!getUInt32(*p,&result.version.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"kind")){
        result.kind.present=true;if(!getUInt32(*p,&result.kind.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"opcode")){
        result.opcode.present=true;if(!getUInt32(*p,&result.opcode.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"id")){
        result.id.present=true;if(!getUInt32(*p,&result.id.value))return false;
    }
    if(const Proto::Value* p=Proto::field(v,"body")){
        result.body.present=true;if(!fromValue(*p,&result.body.value))return false;
    }
    *out=result;return true;
}
Proto::Value toValue(const IpcEnvelope& v){
    Proto::Value out;out.type=Proto::Value::Map;(void)v;
    if(v.version.present)Proto::add(&out,"version",jsonUInt(v.version.value));
    if(v.kind.present)Proto::add(&out,"kind",jsonUInt(v.kind.value));
    if(v.opcode.present)Proto::add(&out,"opcode",jsonUInt(v.opcode.value));
    if(v.id.present)Proto::add(&out,"id",jsonUInt(v.id.value));
    if(v.body.present)Proto::add(&out,"body",toValue(v.body.value));
    return out;
}
}
