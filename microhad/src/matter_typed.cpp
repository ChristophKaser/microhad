#include "matter_client.h"
#include "matter_typed.h"
#include "thermostat.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
namespace {
using namespace MatterProto;
bool oneOf(const std::string& value,const char* words){
    while(*words){const char* end=std::strchr(words,' ');const size_t size=end?static_cast<size_t>(end-words):std::strlen(words);if(value.size()==size&&value.compare(0,size,words,size)==0)return true;if(!end)break;words=end+1;}
    return false;
}
bool errorValid(const std::string& s){return oneOf(s,"malformed not_ready busy stale_session stale_generation timeout cancelled transport storage capacity accessory unsupported not_found stale_revision");}
bool channel(const Optional<uint32_t>& v){return !v.present||(v.value>=11&&v.value<=26);}
bool status(const ThreadStatus& s){return s.enabled.present&&s.available.present&&s.configured.present&&s.attached.present&&s.linux_ipv6_or_tun.present&&!s.linux_ipv6_or_tun.value&&s.state.present&&oneOf(s.state.value,"awaiting_coordinator detached attaching attached channel_mismatch error")&&channel(s.zigbee_channel)&&channel(s.dataset_channel);}
bool status(const MatterStatus& s){return s.enabled.present&&s.available.present&&s.controller_initialized.present&&s.fabric_ready.present&&s.state.present&&oneOf(s.state.value,"running awaiting_coordinator")&&(!s.commissioning_session.present||s.commissioning_session.value);}
bool session(const CommissioningSession& s){return s.session.present&&s.session.value&&s.state.present&&oneOf(s.state.value,"waiting_ble pase provisioning waiting_thread case completing complete cancelled failed timed_out")&&s.deadline_remaining_seconds.present&&s.deadline_remaining_seconds.value<=900&&(!s.error.present||oneOf(s.error.value,"malformed not_ready busy stale_session stale_generation timeout cancelled transport storage capacity accessory"))&&(!s.node.present||s.node.value);}
bool isNull(const ReadingValue& v){return v.null_value.present&&v.null_value.value;}
bool numeric(const MeasurementValue& v,double* n){if(v.number.present){*n=v.number.value;return std::isfinite(*n);}if(v.integer.present){*n=v.integer.value;return true;}{ return false; }}
bool reading(const AttributeReading& d,MatterReading* out,bool report){
    if(!d.node.present||d.node.value<2||!d.endpoint.present||d.endpoint.value>65534||!d.cluster.present||!d.attribute.present||!d.value.present){ return false; }
    const AttributeValue& a=d.value.value;if(!a.type.present||!a.value.present){ return false; }
    const ReadingValue& v=a.value.value;const uint32_t cluster=d.cluster.value;
    if(cluster!=513&&d.attribute.value!=(cluster==47?12U:0U)){ return false; }
    MatterReading r;r.node=MatterWire::hexId(d.node.value);r.endpoint=d.endpoint.value;r.cluster=cluster;r.attribute=d.attribute.value;
    if(cluster==513){if(!thermostatWireValue(d.attribute.value,a)){ return false; }r.thermostatValue=a;r.known=!isNull(v);r.unit=a.type.value;}
    else if(cluster==6||cluster==69||(!report&&(cluster==3||cluster==30||cluster==56||cluster==62))){if(a.type.value!="bool"||!v.boolean.present){ return false; }r.known=true;r.booleanValue=v.boolean.value;}
    else if(cluster==8||cluster==47){if(a.type.value!="u8"){ return false; }r.known=!isNull(v);if(r.known){if(!v.unsigned_value.present||v.unsigned_value.value>(cluster==47?200U:254U)){ return false; }r.numberValue=v.unsigned_value.value;}}
    else if(cluster==91){
        if(a.type.value!="air_quality"||!v.measurement.present){ return false; }
        const Measurement& m=v.measurement.value;if(!m.value.present||!m.value.value.text.present||!m.quality.present||m.unit.present||m.mode.present||m.transitions.present){ return false; }
        r.textValue=m.value.value.text.value;r.known=m.quality.value=="measured";
        if(!(r.known&&oneOf(r.textValue,"good fair moderate poor very_poor extremely_poor"))&&!(m.quality.value=="unknown"&&r.textValue=="unknown")){ return false; }
    }else if(cluster==1026||cluster==1029||cluster==1037||cluster==1066){
        if(a.type.value!="measurement"||!v.measurement.present){ return false; }
        const Measurement& m=v.measurement.value;if(!m.value.present||!m.unit.present||!m.quality.present||m.mode.present||m.transitions.present){ return false; }
        const MeasurementValue& n=m.value.value;r.known=!(n.null_value.present&&n.null_value.value);r.unit=m.unit.value;
        if(m.quality.value!=(r.known?"measured":"unknown")){ return false; }
        if(cluster==1026?r.unit!="celsius":cluster==1029?r.unit!="percent":!oneOf(r.unit,"ppm ppb ppt mg/m3 ug/m3 ng/m3 particles/m3 particles/ft3 Bq/m3 dB/m")){ return false; }
        if(r.known&&(!numeric(n,&r.numberValue)||r.numberValue<(cluster==1026?-327.68:0)||r.numberValue>(cluster==1026?327.67:cluster==1029?100:3.402823466e38))){ return false; }
    }else { return false; }
    if(out){*out=r;}return true;
}
bool list(const Optional<UInt32List>& p,bool descriptor){if(!p.present)return !descriptor;if(p.value.items.size()>64){ return false; }bool found=false;for(size_t i=0;i<p.value.items.size();++i){if(i&&p.value.items[i]<=p.value.items[i-1]){ return false; }if(p.value.items[i]==29)found=true;}return !descriptor||found;}
bool nullUint(const NullableUint& v){return v.null_value.present&&v.null_value.value;}
bool hexText(const std::string& s){for(size_t i=0;i<s.size();++i)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))){ return false; }return true;}
bool device(const MatterDevice& d,MatterEndpoint* out,bool identity){
    if(!d.node.present||d.node.value<2||!d.state.present||!oneOf(d.state.value,"complete recovery_required commissioning")||!d.online.present||(d.online.value&&d.state.value!="complete")||!d.id.present||!d.id.value.text.present||!d.endpoint.present){ return false; }
    const std::string& id=d.id.value.text.value;
    if(id.size()!=(identity?40U:45U)||id.compare(0,7,"matter:")||id[23]!=':'||!hexText(id.substr(7,16))||id.substr(7,16)=="0000000000000000"||id.substr(24,16)!=MatterWire::hexId(d.node.value)){ return false; }
    if(identity){if(!nullUint(d.endpoint.value)||(d.servers.present&&!d.servers.value.items.empty())||(d.clients.present&&!d.clients.value.items.empty())||(d.device_types.present&&!d.device_types.value.items.empty())){ return false; }}
    else if(!d.endpoint.value.integer.present||d.endpoint.value.integer.value>65534||id[40]!=':'||!hexText(id.substr(41,4))||strtoul(id.substr(41,4).c_str(),0,16)!=d.endpoint.value.integer.value||!list(d.servers,true)||!list(d.clients,false)){ return false; }
    if(d.device_types.present&&d.device_types.value.items.size()>64){ return false; }
    if((d.manufacturer.present&&d.manufacturer.value.size()>64)||(d.model.present&&d.model.value.size()>64)){ return false; }
    if(d.rssi_dbm.present&&!(d.rssi_dbm.value.null_value.present&&d.rssi_dbm.value.null_value.value)&&(!d.rssi_dbm.value.integer.present||d.rssi_dbm.value.integer.value< -128||d.rssi_dbm.value.integer.value>0)){ return false; }
    if(out){out->id=id;out->node=MatterWire::hexId(d.node.value);out->state=d.state.value;out->endpoint=identity?0xffff:static_cast<unsigned short>(d.endpoint.value.integer.value);out->online=d.online.value;if(d.servers.present)out->servers.assign(d.servers.value.items.begin(),d.servers.value.items.end());if(d.clients.present)out->clients.assign(d.clients.value.items.begin(),d.clients.value.items.end());if(d.device_types.present)out->deviceTypes.assign(d.device_types.value.items.begin(),d.device_types.value.items.end());if(d.manufacturer.present)out->manufacturer=d.manufacturer.value;if(d.model.present)out->model=d.model.value;out->rssiDbm=d.rssi_dbm.present&&d.rssi_dbm.value.integer.present?static_cast<int>(d.rssi_dbm.value.integer.value):1;}
    return true;
}
bool inventory(const InventoryRecord& v,std::string* kind,unsigned* count,MatterEndpoint* out){
    if(!v.kind.present||!oneOf(v.kind.value,"identity endpoint commit")||!v.endpoint_count.present||v.endpoint_count.value>32||!v.device.present||!device(v.device.value,out,v.kind.value!="endpoint")){ return false; }
    if(kind){*kind=v.kind.value;}if(count){*count=v.endpoint_count.value;}if(out){out->metadataComplete=v.kind.value=="commit"&&v.endpoint_count.value>0;}return true;
}
bool inventoryNodes(const InventoryNodeManifest& v,std::vector<std::string>* out){
    if(!v.nodes.present||v.nodes.value.items.size()>64){ return false; }uint64_t previous=1;
    for(size_t i=0;i<v.nodes.value.items.size();++i){const InventoryNode& n=v.nodes.value.items[i];if(!n.node.present||n.node.value<=previous){ return false; }previous=n.node.value;if(out)out->push_back(MatterWire::hexId(n.node.value));}return true;
}
bool button(const ButtonEvent& v,MatterButtonEvent* out){if(!v.node.present||v.node.value<2||!v.endpoint.present||v.endpoint.value>65534||!v.event.present||v.event.value>5||(v.position.present&&v.position.value>254)||(v.press_count.present&&v.press_count.value>32)){ return false; }if(out){out->node=MatterWire::hexId(v.node.value);out->endpoint=v.endpoint.value;out->event=v.event.value;out->position=v.position.present?static_cast<int>(v.position.value):-1;out->pressCount=v.press_count.present?static_cast<int>(v.press_count.value):-1;}return true;}
bool job(const DeviceOperationJob& j){if(!j.job.present||!j.job.value||!j.state.present){ return false; }if(j.state.value=="queued"||j.state.value=="running")return !j.error.present&&!j.result.present;if(j.state.value=="failed")return j.error.present&&errorValid(j.error.value)&&!j.result.present;return j.state.value=="complete"&&!j.error.present&&j.result.present&&reading(j.result.value,0,false);}
bool boundedNullable(const Optional<NullableUint>& n,uint64_t minimum,uint64_t maximum){return n.present&&(nullUint(n.value)||(n.value.integer.present&&n.value.integer.value>=minimum&&n.value.integer.value<=maximum));}
bool subscriptions(const SubscriptionHealth& h){if(!h.node.present||h.node.value<2||!h.restore_scheduled.present||!h.intermittent.present||!h.reachability.present||!oneOf(h.reachability.value,"online offline sleeping")||!boundedNullable(h.next_cursor,1,384)||!h.subscriptions.present||h.subscriptions.value.items.size()>6){ return false; }for(size_t i=0;i<h.subscriptions.value.items.size();++i){const Subscription& s=h.subscriptions.value.items[i];if(!s.endpoint.present||s.endpoint.value>65534||!s.cluster.present||!s.attribute.present||!boundedNullable(s.id,0,0xffffffffUL)||!s.max_interval_seconds.present||s.max_interval_seconds.value<1||s.max_interval_seconds.value>65535||!boundedNullable(s.last_report_age_seconds,0,0xffffffffUL)||!s.retry_in_seconds.present||!s.failures.present||s.failures.value>255||!s.state.present||!oneOf(s.state.value,"active overdue pending establishing")){ return false; }}return true;}
const EventPayload* eventData(const MessageBody& b,const char* name){
    if(!b.event.present){return 0;}const EventNotification& e=b.event.value;
    return e.schema.present&&e.schema.value==1&&e.event.present&&e.event.value==name&&e.data.present?&e.data.value:0;
}

}
namespace MatterWire {
std::string hexId(uint64_t n){const char* digits="0123456789abcdef";std::string out(16,'0');for(unsigned i=16;i>0;--i){out[i-1]=digits[n&15];n>>=4;}return out;}
bool validResponse(unsigned char op,const MessageBody& b){
    if(op==1){return b.matter.present&&status(b.matter.value);}
    if(op==2){return b.thread.present&&status(b.thread.value);}
    if(op<3||op>11||!b.reply.present){ return false; }const OperationResponse& r=b.reply.value;
    if(!r.schema.present||r.schema.value!=1||!r.ok.present){ return false; }
    if(!r.ok.value)return r.error.present&&errorValid(r.error.value)&&(r.error.value!="stale_revision"||op==10)&&!r.result.present;
    if(r.error.present||!r.result.present){ return false; }const OperationResult& v=r.result.value;
    if(op<=5)return v.session.present&&session(v.session.value);
    if(op==6)return v.ble_attach_result.present&&v.ble_attach_result.value.generation.present&&v.ble_attach_result.value.generation.value;
    if(op==7)return v.ble_indication_result.present&&v.ble_indication_result.value.sequence.present&&v.ble_indication_result.value.sequence.value;
    if(op==8)return v.ble_write_complete_result.present&&v.ble_write_complete_result.value.write.present&&v.ble_write_complete_result.value.write.value;
    if(op==9)return v.thread.present&&status(v.thread.value);
    return v.job.present?(job(v.job.value)&&(op!=11||v.job.value.state.value=="queued")):op==10&&v.subscription_health.present&&subscriptions(v.subscription_health.value);
}
bool validEvent(const MessageBody& b){
    if(!b.event.present){ return false; }const EventNotification& e=b.event.value;if(!e.schema.present||e.schema.value!=1||!e.event.present||!e.data.present){ return false; }
    const EventPayload& d=e.data.value;const std::string& name=e.event.value;
    if(name=="commissioning")return d.session.present&&session(d.session.value);
    if(name=="thread")return d.thread.present&&status(d.thread.value);
    if(name=="matter_report")return d.reading.present&&reading(d.reading.value,0,true);
    if(name=="matter_inventory")return d.inventory.present&&inventory(d.inventory.value,0,0,0);
    if(name=="matter_inventory_nodes")return d.inventory_nodes.present&&inventoryNodes(d.inventory_nodes.value,0);
    if(name=="matter_button")return d.button.present&&button(d.button.value,0);
    if(name!="ble_write"||!d.ble_write.present){ return false; }const BleWriteEvent& w=d.ble_write.value;
    return w.session.present&&w.session.value&&w.generation.present&&w.generation.value&&w.write.present&&w.write.value&&w.data.present&&!w.data.value.empty()&&w.data.value.size()<=244;
}
}
bool MatterChannel::acceptedOperationJob(unsigned char op,const Body& b,uint64_t* id){*id=0;if((op!=10&&op!=11)||!MatterWire::validResponse(op,b)||!b.reply.value.ok.value||!b.reply.value.result.value.job.present){ return false; }const MatterProto::DeviceOperationJob& j=b.reply.value.result.value.job.value;if(j.state.value!="queued"){ return false; }*id=j.job.value;return true;}
bool MatterChannel::operationStatus(const Body& b,uint64_t id,bool* terminal,bool* failed,std::string* error){*terminal=false;*failed=false;error->clear();if(!MatterWire::validResponse(10,b)){ return false; }const MatterProto::OperationResponse& r=b.reply.value;if(!r.ok.value){*terminal=*failed=true;*error=r.error.value;return true;}if(!r.result.value.job.present){ return false; }const MatterProto::DeviceOperationJob& j=r.result.value.job.value;if(j.job.value!=id){ return false; }*terminal=j.state.value=="failed"||j.state.value=="complete";*failed=j.state.value=="failed";if(*failed)*error=j.error.value;return true;}
MatterChannel::Body MatterChannel::operationPollBody(uint64_t id){Body b;if(!id)return b;MatterProto::DeviceControlRequest r;r.schema=1;r.action=std::string("job");r.job=id;b.device_control_request=r;return b;}
bool MatterChannel::threadChannel(const Body& in,unsigned char channel,Body* out){if(channel<11||channel>26||!in.thread_control_request.present){ return false; }*out=in;if(out->thread_control_request.value.action.value!="retry")out->thread_control_request.value.zigbee_channel=channel;return true;}
bool MatterChannel::deviceResult(const Body& b,uint64_t id,MatterReading* r){if(!r||!MatterWire::validResponse(10,b)||!b.reply.value.ok.value||!b.reply.value.result.value.job.present){ return false; }const MatterProto::DeviceOperationJob& j=b.reply.value.result.value.job.value;return j.state.value=="complete"&&j.job.value==id&&reading(j.result.value,r,false);}
bool MatterChannel::deviceReportEvent(const Body& b,MatterReading* r){const EventPayload* e=eventData(b,"matter_report");return r&&e&&e->reading.present&&reading(e->reading.value,r,true);}
bool MatterChannel::deviceButtonEvent(const Body& b,MatterButtonEvent* v){const EventPayload* e=eventData(b,"matter_button");return v&&e&&e->button.present&&button(e->button.value,v);}
bool MatterChannel::inventoryEvent(const Body& b,std::string* kind,unsigned* count,MatterEndpoint* out){const EventPayload* e=eventData(b,"matter_inventory");return e&&e->inventory.present&&inventory(e->inventory.value,kind,count,out);}
bool MatterChannel::inventoryNodesEvent(const Body& b,std::vector<std::string>* out){out->clear();const EventPayload* e=eventData(b,"matter_inventory_nodes");return e&&e->inventory_nodes.present&&inventoryNodes(e->inventory_nodes.value,out);}
uint64_t MatterChannel::relayResponseSession(const Body& b){return b.reply.present&&b.reply.value.ok.value&&b.reply.value.result.value.session.present?b.reply.value.result.value.session.value.session.value:0;}
uint32_t MatterChannel::relayGeneration(const Body& b){return b.reply.present&&b.reply.value.ok.value&&b.reply.value.result.value.ble_attach_result.present?b.reply.value.result.value.ble_attach_result.value.generation.value:0;}
bool MatterChannel::relayEvent(const Body& b,uint64_t id,uint32_t generation){if(!MatterWire::validEvent(b)){ return false; }const MatterProto::EventPayload& e=b.event.value.data.value;if(e.session.present)return e.session.value.session.value==id;return e.ble_write.present&&generation&&e.ble_write.value.generation.value==generation&&e.ble_write.value.session.value==id;}
