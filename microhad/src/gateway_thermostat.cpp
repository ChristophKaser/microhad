#include "gateway.h"
#include "zcl.h"
#include "json_util.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>

namespace {
void put16(std::vector<unsigned char>* bytes, unsigned int value) {
    bytes->push_back(static_cast<unsigned char>(value)); bytes->push_back(static_cast<unsigned char>(value >> 8));
}
void put32(std::vector<unsigned char>* bytes, unsigned long value) {
    for (unsigned int i=0;i<4;++i) bytes->push_back(static_cast<unsigned char>(value>>(8*i)));
}
unsigned short get16(const std::vector<unsigned char>& bytes, unsigned long at) {
    return bytes[at] | (static_cast<unsigned short>(bytes[at+1])<<8);
}
bool sensorFresh(const Device& source, long now, unsigned long minutes) {
    return source.hasTemperature && source.temperatureKnown && source.temperatureObservation.at>0 &&
        now>=source.temperatureObservation.at && now-source.temperatureObservation.at<=static_cast<long>(minutes*60) &&
        source.temperatureCelsius>=0 && source.temperatureCelsius<=99.9;
}
bool thermostatEndpoint(const Device& device, unsigned short cluster, unsigned char* endpoint) {
    for (unsigned long i=0;i<device.endpoints.size();++i) {
        const EndpointDescriptor& e=device.endpoints[i];
        if (std::find(e.inputClusters.begin(),e.inputClusters.end(),cluster)!=e.inputClusters.end()) {
            *endpoint=e.endpoint; return true;
        }
    }
    return false;
}
}
void Gateway::queueThermostatOperation(const Device& device, unsigned pendingId, unsigned short cluster,
                                      unsigned char command, const std::vector<unsigned char>& payload) {
    if (thermostatOperations_.size()>=128) return;
    unsigned char endpoint=0;
    if (!thermostatEndpoint(device,cluster,&endpoint)) return;
    for (std::deque<ThermostatOperation>::const_iterator it=thermostatOperations_.begin();it!=thermostatOperations_.end();++it)
        if (it->address==device.ieeeAddress && it->pendingId==pendingId) return;
    ThermostatOperation op; op.address=device.ieeeAddress; op.pendingId=pendingId; op.cluster=cluster;
    op.endpoint=endpoint; op.command=command; op.payload=payload;
    devices_[device.ieeeAddress].thermostat.pending.insert(op.pendingId);
    markViewDirty();
    if (command==2 || command==0x81) {
        std::deque<ThermostatOperation>::iterator place=thermostatOperations_.begin();
        if (place!=thermostatOperations_.end() && place->deadline!=0) ++place;
        while (place!=thermostatOperations_.end() && (place->command==2 || place->command==0x81)) ++place;
        thermostatOperations_.insert(place,op);
    } else thermostatOperations_.push_back(op);
}
void Gateway::queueThermostatReadings(const Device& device) {
    if (!isThermostat(device) || device.source!="zigbee") return;
    unsigned long count; const ThermostatAttribute* list=thermostatAttributes(&count);
    for (unsigned long i=0;i<count;++i) {
        const ThermostatAttribute& a=list[i];
        if (device.thermostat.unsupported.count(a.id)) continue;
        /* Probe features on unknown firmware; a successful read establishes support. */
        if (a.cluster!=513 && !isTrvzb(device)) continue;
        if (a.minimumVersion[0] && !device.softwareBuildId.empty() && !thermostatSupported(device,a)) continue;
        std::vector<unsigned char> payload; put16(&payload,a.attribute);
        queueThermostatOperation(device,a.id,a.cluster,0,payload);
    }
    for (int day=0;day<7;++day) {
        std::vector<unsigned char> payload; payload.push_back(static_cast<unsigned char>(1<<day)); payload.push_back(isTrvzb(device)?1:3);
        queueThermostatOperation(device,2*ThermostatField::Count+day,0x0201,0x82,payload);
    }
}
bool Gateway::setThermostat(const std::string& address, const std::string& field,
                            const std::string& value, std::string* error, MatterProto::MessageBody* matterRequest) {
    std::map<std::string,Device>::iterator it=devices_.find(resolveDeviceId(address));
    if (it==devices_.end() || !isThermostat(it->second)) { *error="device is not a thermostat"; return false; }
    Device& device=it->second; ThermostatState& state=device.thermostat;
    if(matterRequest)*matterRequest=MatterProto::MessageBody();
    if (device.source=="matter") {
        std::vector<unsigned char> encoded;
        bool schedule=field.size()==10 && field.compare(0,9,"schedule_")==0 && field[9]>='0' && field[9]<='6';
        if (!matterRequest) { *error="Matter worker is required"; return false; }
        if (device.thermostat.pending.count(thermostatPendingId(field))) { *error="this setting has a pending request"; return false; }
        if (schedule ? (value=="__read__" ? !state.scheduleSupported : !thermostatScheduleForDevice(device,value,field[9]-'0',&encoded)) : !thermostatEncode(device,field,value,&encoded,error)) {
            if(schedule) *error="invalid or unsupported thermostat schedule";
            return false;
        }
        MatterProto::DeviceCommandRequest request;request.schema=1;request.command=std::string("thermostat");request.node=strtoull(device.matterNode.c_str(),0,16);request.endpoint=device.matterEndpoint;request.field=field;request.value=value;
        matterRequest->device_command_request=request; state.pending.insert(thermostatPendingId(field)); state.error.clear(); markViewDirty(); return true;
    }
    unsigned char endpoint=0;
    if (!thermostatEndpoint(device,0x0201,&endpoint)) { *error="thermostat interview is incomplete"; return false; }
    if (field=="stale_minutes") {
        double minutes;
        if (!thermostatNumber(value,&minutes) || minutes<5 || minutes>1440 || minutes!=std::floor(minutes)) {
            *error="stale timeout must be 5–1440 whole minutes"; return false;
        }
        state.staleMinutes=static_cast<unsigned long>(minutes); savePersistentState(); markViewDirty(); return true;
    }
    if (!online_ || !networkFormed_ || coordinatorInitializationStep_!=0 || device.networkAddress==0xffff) {
        *error="Zigbee network or device address is unavailable"; return false;
    }
    if (thermostatOperations_.size()>=120) { *error="thermostat queue is full; retry shortly"; return false; }
    if (field=="temperature_source") {
        for (std::deque<ThermostatOperation>::const_iterator op=thermostatOperations_.begin();op!=thermostatOperations_.end();++op)
            if (op->address==device.ieeeAddress && op->pendingId==ThermostatField::temperature_sensor_select && op->deadline!=0) {
                *error="wait for the current temperature source confirmation before changing sources"; return false;
            }
        if (!thermostatSupported(device,*thermostatAttribute(ThermostatField::temperature_sensor_select)) ||
            (value!="builtin" && !thermostatSupported(device,*thermostatAttribute(ThermostatField::external_temperature_input))) ||
            !thermostatEndpoint(device,0xfc11,&endpoint)) { *error="external temperature is unavailable"; return false; }
        if (value!="builtin" && value!="manual") {
            std::map<std::string,Device>::const_iterator source=devices_.find(value);
            if (source==devices_.end() || source->first==device.ieeeAddress || !source->second.hasTemperature ||
                isThermostat(source->second) || (source->second.source=="matter" && source->second.matterEndpoint<0)) {
                *error="select a measured temperature sensor endpoint"; return false;
            }
        }
        /* Cancel queued work from the old source, including its mode change.
           An in-flight write may still complete; the new selection follows it. */
        for (std::deque<ThermostatOperation>::iterator op=thermostatOperations_.begin();op!=thermostatOperations_.end();) {
            if (op->address==device.ieeeAddress && op->deadline==0 &&
                (op->pendingId==ThermostatField::external_temperature_input || op->pendingId==ThermostatField::temperature_sensor_select)) { state.pending.erase(op->pendingId); op=thermostatOperations_.erase(op); }
            else ++op;
        }
        state.sourceId=(value=="builtin" || value=="manual") ? "" : value;
        state.lastForwardAt=0; state.lastForwardValue=-1; state.error.clear();
        state.status=state.sourceId.empty() ? "Awaiting temperature source confirmation" : "Waiting for a fresh source reading";
        if (state.sourceId.empty()) {
            std::vector<unsigned char> payload;
            if (!thermostatEncode(device,"temperature_sensor_select",value=="builtin" ? "0" : "1",&payload,error)) return false;
            queueThermostatOperation(device,ThermostatField::temperature_sensor_select,0xfc11,2,payload);
        }
        savePersistentState(); markViewDirty(); return true;
    }
    for (std::deque<ThermostatOperation>::const_iterator op=thermostatOperations_.begin();op!=thermostatOperations_.end();++op)
        if (op->address==device.ieeeAddress && op->pendingId==thermostatPendingId(field)) { *error="this thermostat setting already has a pending request"; return false; }
    std::vector<unsigned char> payload;
    unsigned short cluster=0x0201; unsigned char command=2;
    if (field.size()==10 && field.compare(0,9,"schedule_")==0 && field[9]>='0' && field[9]<='6') {
        if(value=="__read__") {
            payload.push_back(static_cast<unsigned char>(1<<(field[9]-'0'))); payload.push_back(isTrvzb(device)?1:3); command=0x82;
        } else {
            if (!thermostatScheduleForDevice(device,value,field[9]-'0',&payload)) { *error="invalid or unsupported thermostat schedule"; return false; }
            command=0x81;
        }
    } else {
        const ThermostatAttribute* a=thermostatAttribute(field);
        if (!a) { *error="unknown thermostat setting"; return false; }
        if (!thermostatEncode(device,field,value,&payload,error)) return false;
        cluster=a->cluster;
        if (!thermostatEndpoint(device,cluster,&endpoint)) { *error="setting cluster is unavailable"; return false; }
        if (!state.sourceId.empty() && (field=="external_temperature_input" || field=="temperature_sensor_select")) {
            *error="switch to manual or built-in temperature source first"; return false;
        }
    }
    queueThermostatOperation(device,thermostatPendingId(field),cluster,command,payload);
    state.error.clear(); state.status="Awaiting device confirmation"; markViewDirty(); return true;
}
long Gateway::thermostatDeadline() const {
    if (!online_ || !networkFormed_ || coordinatorInitializationStep_!=0) return 0;
    if (interviewRequestInFlight_) return 0;
    if (!thermostatOperations_.empty()) return thermostatOperations_.front().deadline ? thermostatOperations_.front().deadline : time(0)+1;
    for (std::map<std::string,Device>::const_iterator it=devices_.begin();it!=devices_.end();++it)
        if (isTrvzb(it->second) && !it->second.thermostat.sourceId.empty()) return time(0)+1;
    return 0;
}
void Gateway::pollThermostats() {
    if (!online_ || !networkFormed_ || coordinatorInitializationStep_!=0) return;
    long now=time(0);
    for (std::map<std::string,Device>::iterator it=devices_.begin();it!=devices_.end();++it) {
        Device& valve=it->second; ThermostatState& t=valve.thermostat;
        if (!isTrvzb(valve) || t.sourceId.empty()) continue;
        std::map<std::string,Device>::const_iterator source=devices_.find(t.sourceId);
        bool fresh=source!=devices_.end() && !isThermostat(source->second) && sensorFresh(source->second,now,t.staleMinutes);
        long observed=source==devices_.end() ? 0 : source->second.temperatureObservation.at;
        if (t.sourceObservedAt!=observed) { t.sourceObservedAt=observed; markViewDirty(); }
        bool pending=false;
        for (std::deque<ThermostatOperation>::iterator op=thermostatOperations_.begin();op!=thermostatOperations_.end();) {
            if (op->address==it->first && (op->pendingId==ThermostatField::external_temperature_input || op->pendingId==ThermostatField::temperature_sensor_select)) {
                if (!fresh && op->deadline==0 && (op->pendingId==ThermostatField::external_temperature_input || (op->command==0 || (op->payload.size()>3 && op->payload[3]==1)))) {
                    t.pending.erase(op->pendingId); op=thermostatOperations_.erase(op); continue;
                }
                pending=true;
            }
            ++op;
        }
        std::string status;
        const double mode=t.values.get(ThermostatField::temperature_sensor_select,-1);
        if (!fresh) {
            std::string reason=source==devices_.end() ? "Source missing" : observed==0 ? "Waiting for a fresh source reading" : "Source reading stale or invalid";
            bool local=mode==0;
            status=reason+(local ? "; built-in sensor confirmed" : "; awaiting built-in fallback confirmation");
            if (!local && !pending && (t.lastForwardAt==0 || now-t.lastForwardAt>=60)) {
                std::vector<unsigned char> payload; std::string error;
                if (thermostatEncode(valve,"temperature_sensor_select","0",&payload,&error)) {
                    queueThermostatOperation(valve,ThermostatField::temperature_sensor_select,0xfc11,2,payload); t.lastForwardAt=now;
                } else t.error=error;
            }
        } else {
            double temperature=std::floor(source->second.temperatureCelsius*10+.5)/10;
            bool remote=mode==1;
            status=remote ? "Linked sensor active" : "Awaiting external sensor confirmation";
            if (!pending && (t.lastForwardAt==0 || now-t.lastForwardAt>=60)) {
                std::vector<unsigned char> payload; std::ostringstream value; std::string error;
                if (temperature!=t.lastForwardValue || !remote || now-t.lastForwardAt>=300) {
                    value<<temperature;
                    if (thermostatEncode(valve,"external_temperature_input",value.str(),&payload,&error)) {
                        queueThermostatOperation(valve,ThermostatField::external_temperature_input,0xfc11,2,payload); t.lastForwardAt=now;
                    } else t.error=error;
                }
            }
        }
        if (t.status!=status) { t.status=status; markViewDirty(); }
    }
    if (thermostatOperations_.empty()) return;
    ThermostatOperation& op=thermostatOperations_.front();
    std::map<std::string,Device>::iterator device=devices_.find(op.address);
    if (device==devices_.end() || !isThermostat(device->second)) { thermostatOperations_.pop_front(); return; }
    if (op.deadline!=0 && now<op.deadline) return;
    if (op.attempts>=3) {
        device->second.thermostat.error="No confirmation for "+thermostatPendingName(op.pendingId);
        device->second.thermostat.status="Request timed out";
        device->second.thermostat.pending.erase(op.pendingId);
        thermostatOperations_.pop_front(); markViewDirty(); return;
    }
    if (interviewRequestInFlight_) return;
    if (!op.attempts) op.sequence=++zclTransactionSequence_;
    std::vector<unsigned char> frame;
    frame.push_back(op.command>=0x80 ? 0x01 : 0x00);
    frame.push_back(op.sequence); frame.push_back(op.command&0x7f);
    if (op.readback && op.command==2) { frame[2]=0; frame.insert(frame.end(),op.payload.begin(),op.payload.begin()+2); }
    else if (op.readback && op.command==0x81) {
        frame[2]=2; frame.push_back(op.payload[1]); frame.push_back(op.payload[2]);
    } else frame.insert(frame.end(),op.payload.begin(),op.payload.end());
    ++op.attempts; op.deadline=now+20;
    sendApsUnicast(device->second.networkAddress,0x0104,op.cluster,1,op.endpoint,frame);
}
bool Gateway::handleThermostatMessage(const std::string& address, unsigned char endpoint, unsigned short cluster,
    unsigned char control, unsigned short manufacturer, unsigned char sequence, unsigned char command,
    const std::vector<unsigned char>& payload) {
    Device& device=devices_[address];
    if ((control&8)==0 && (control&3)==0 && cluster==0x000a && command==0 && manufacturer==0) {
        /* TRVZB requests Unix UTC seconds (not the ZCL-2000 epoch). */
        if (payload.empty() || payload.size()%2) return true;
        std::vector<unsigned char> reply; reply.push_back(0x18); reply.push_back(sequence); reply.push_back(1);
        time_t now=time(0); struct tm local; localtime_r(&now,&local);
        for (unsigned long i=0;i<payload.size();i+=2) {
            unsigned short a=get16(payload,i); put16(&reply,a);
            if (a==0 || a==2 || a==7) {
                reply.push_back(0); reply.push_back(a==2 ? 0x2b : 0x23);
                unsigned long epoch=isTrvzb(device) ? 0 : 946684800UL;
                put32(&reply,static_cast<unsigned long>(a==0 ? now-epoch : a==2 ? local.tm_gmtoff : now-epoch+local.tm_gmtoff));
            } else if (a==1) { reply.push_back(0); reply.push_back(0x18); reply.push_back(3); }
            else reply.push_back(0x86);
        }
        sendApsUnicast(device.networkAddress,0x0104,cluster,1,endpoint,reply); return true;
    }
    if (cluster==0x0020 && (control&0x0b)==9 && command==0 && manufacturer==0) {
        std::vector<unsigned char> reply; reply.push_back(0x11); reply.push_back(sequence); reply.push_back(0);
        reply.push_back(1); put16(&reply,120);
        sendApsUnicast(device.networkAddress,0x0104,cluster,1,endpoint,reply); return true;
    }
    if (cluster!=0x0201 && (cluster!=0xfc11 || !isTrvzb(device))) return false;
    /* Configure Reporting belongs to the existing interview state machine. */
    if ((control&3)==0 && command==7) {
        if(!thermostatOperations_.empty()) {
            const ThermostatOperation& reporting=thermostatOperations_.front();
            if(reporting.command==6 && reporting.address==address && reporting.endpoint==endpoint && reporting.cluster==cluster && reporting.sequence==sequence && reporting.deadline) {
                device.thermostat.pending.erase(reporting.pendingId); thermostatOperations_.pop_front();markViewDirty();return true;
            }
        }
        return false;
    }
    unsigned char expectedEndpoint=0;
    if (!thermostatEndpoint(device,cluster,&expectedEndpoint) || endpoint!=expectedEndpoint ||
        manufacturer!=0 || !(control&8)) return true;
    bool matched=!thermostatOperations_.empty() && thermostatOperations_.front().address==address &&
        thermostatOperations_.front().endpoint==endpoint && thermostatOperations_.front().cluster==cluster &&
        thermostatOperations_.front().sequence==sequence && thermostatOperations_.front().deadline!=0;
    bool success=false, completed=false;
    if ((control&3)==0 && command==0x04 && matched) {
        ThermostatOperation& op=thermostatOperations_.front();
        if (op.command==2 && !op.readback) {
            success=payload.size()==1 && payload[0]==0;
            if (success) { op.readback=true; op.attempts=0; op.deadline=0; }
            else {
                if (payload.size()>=3 && payload[0]==0x86 && get16(payload,1)==get16(op.payload,0))
                    device.thermostat.unsupported.insert(static_cast<ThermostatField::Id>(op.pendingId));
                completed=true;
            }
        }
    } else if ((control&3)==0 && command==0x0b && matched && payload.size()==2) {
        ThermostatOperation& op=thermostatOperations_.front();
        if (payload[0]==(op.command&0x7f) && payload[1]!=0) { completed=true; if(op.command>=0x80)device.thermostat.scheduleSupported=false; }
        else if (payload[0]==1 && op.command==0x81 && !op.readback && payload[1]==0) {
            op.readback=true; op.attempts=0; op.deadline=0;
        }
    } else if ((control&3)==1 && cluster==0x0201 && command==0) {
        success=applyThermostatSchedule(&device,payload);
        if (matched && success) {
            ThermostatOperation& op=thermostatOperations_.front();
            if (op.command==0x82) completed=payload[1]==op.payload[0];
            else if (op.command==0x81) {
                completed=payload[1]==op.payload[1]; success=payload==op.payload;
            }
        }
        markViewDirty();
    } else if ((control&3)==0 && (command==1 || command==0x0a)) {
        unsigned long at=0;
        while (at+3<=payload.size()) {
            unsigned short attribute=get16(payload,at); at+=2;
            unsigned char status=command==1 ? payload[at++] : 0;
            const ThermostatAttribute* a=0; unsigned long count; const ThermostatAttribute* list=thermostatAttributes(&count);
            for (unsigned long i=0;i<count;++i) if (list[i].cluster==cluster && list[i].attribute==attribute) a=&list[i];
            bool target=matched && a && thermostatOperations_.front().pendingId==static_cast<unsigned>(a->id);
            if (status) {
                if (a && status==0x86) { device.thermostat.unsupported.insert(a->id); device.thermostat.values.erase(a->id); }
                if (target) completed=true;
                continue;
            }
            if (at>=payload.size()) break;
            unsigned char type=payload[at++];
            unsigned long size=type==0x23 ? 4 : type==0x21 || type==0x29 || type==0x19 ? 2 :
                type==0x20 || type==0x28 || type==0x10 || type==0x18 || type==0x30 ? 1 : 0;
            if (!size || at+size>payload.size()) break;
            std::vector<unsigned char> value(payload.begin()+at,payload.begin()+at+size); at+=size;
            Device before=device;
            bool applied=applyThermostatAttribute(&device,cluster,attribute,type,value,time(0));
            if (applied && cluster==0x0201 && attribute==0) {
                recordHistory(before,device,ZclObservedTemperature,time(0));
                updateTemperatureAlert(device,before.temperatureKnown,before.temperatureCelsius);
                updateValueAlerts(device,before);
            }
            if(applied && command==1 && !isTrvzb(device) && (attribute==0x11 || attribute==0x12)) {
                std::vector<unsigned char> config=buildZclConfigureReporting(0,attribute,type,10,3600,10);
                config.erase(config.begin(),config.begin()+3);
                queueThermostatOperation(device,ThermostatField::Count+a->id,cluster,6,config);
            }
            if (target && command==1) {
                ThermostatOperation& op=thermostatOperations_.front();
                if (op.command==0 || (op.command==2 && op.readback)) {
                    completed=true; success=applied;
                    if (success && op.command==2) {
                        bool reconnect=op.pendingId==ThermostatField::temperature_sensor_select && value.size()==1 && value[0]==3 && op.payload[3]==1;
                        success=reconnect || (op.payload.size()==value.size()+3 &&
                            std::equal(value.begin(),value.end(),op.payload.begin()+3));
                    }
                }
            }
        }
        if (command==0x0a && !(control&0x10)) {
            std::vector<unsigned char> reply; reply.push_back(0x10); reply.push_back(sequence);
            reply.push_back(0x0b); reply.push_back(command); reply.push_back(0);
            sendApsUnicast(device.networkAddress,0x0104,cluster,1,endpoint,reply);
        }
        markViewDirty();
    }
    if (matched && completed) {
        ThermostatOperation op=thermostatOperations_.front(); thermostatOperations_.pop_front();
        device.thermostat.pending.erase(op.pendingId);
        if (!success && op.command!=0) device.thermostat.error="Device rejected or did not confirm "+thermostatPendingName(op.pendingId);
        else if (success) {
            if (op.command!=0) device.thermostat.error.clear();
            device.thermostat.status="Device confirmed "+thermostatPendingName(op.pendingId);
            if (op.pendingId==ThermostatField::external_temperature_input && op.command==2 && op.readback && !device.thermostat.sourceId.empty()) {
                device.thermostat.lastForwardValue=device.thermostat.values.get(ThermostatField::external_temperature_input,0);
                std::map<std::string,Device>::const_iterator source=devices_.find(device.thermostat.sourceId);
                if (source!=devices_.end() && sensorFresh(source->second,time(0),device.thermostat.staleMinutes) &&
                    std::fabs(std::floor(source->second.temperatureCelsius*10+.5)/10-device.thermostat.lastForwardValue)<.001) {
                    std::vector<unsigned char> bytes; std::string error;
                    if (thermostatEncode(device,"temperature_sensor_select","1",&bytes,&error))
                        queueThermostatOperation(device,ThermostatField::temperature_sensor_select,0xfc11,2,bytes);
                }
            }
        }
        markViewDirty();
    }
    return true;
}

bool Gateway::thermostatResult(const std::string& node, unsigned short endpoint, const std::string& field, const std::string& error) {
    for(std::map<std::string,Device>::iterator it=devices_.begin();it!=devices_.end();++it) {
        Device& d=it->second;
        if(d.source=="matter" && d.matterNode==node && d.matterEndpoint==endpoint && isThermostat(d)) {
            d.thermostat.pending.erase(thermostatPendingId(field)); d.thermostat.error=error;
            d.thermostat.status=error.empty()?"Device confirmed "+field:"Thermostat request failed";
            markViewDirty(); return true;
        }
    }
    return false;
}
