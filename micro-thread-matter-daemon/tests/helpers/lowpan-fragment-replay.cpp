// Isolated test-only receive-path replay. Never linked into the Matter service.
#include "instance/instance.hpp"
#include "thread/mesh_forwarder.hpp"
#include "thread/lowpan.hpp"
#include "net/ip6.hpp"
#include <openthread/thread.h>
#include <cstdio>
#include <cstdlib>

extern "C" void gatewayOtSetDiagnosticLog(void (*)(uint8_t,const char*));
void diag(uint8_t,const char *s){fprintf(stderr,"%s\n",s);}
extern "C" int gatewayOtInit(const char *);
extern "C" otInstance *gatewayOtDiscoveryTestInstance();
int main(int argc, char **argv) {
 setbuf(stdout,NULL); gatewayOtSetDiagnosticLog(diag);
 if(argc!=3 || gatewayOtInit(argv[1]))return 2;
 FILE *f=fopen(argv[2],"rb"); if(!f)return 3;
 uint8_t bytes[4096];size_t length=fread(bytes,1,sizeof(bytes),f);fclose(f);
 ot::Instance &instance=ot::AsCoreType(gatewayOtDiscoveryTestInstance());
 ot::Mac::Addresses addresses;addresses.mSource.SetShort(0x1001);addresses.mDestination.SetShort(0x1000);
 ot::FrameData frame;frame.Init(bytes,length);
 ot::Lowpan::FragmentHeader frag;ot::Error e=frag.ParseFrom(frame);
 printf("fragment parse=%d size=%u offset=%u payload=%u\n",e,frag.GetDatagramSize(),frag.GetDatagramOffset(),frame.GetLength());
 ot::Ip6::Headers headers;e=headers.DecompressFrom(frame,addresses,instance);printf("header priority parse=%d\n",e);
 ot::Message *message=instance.Get<ot::MessagePool>().Allocate(ot::Message::kTypeIp6);
 e=instance.Get<ot::Lowpan::Lowpan>().Decompress(*message,addresses,frame,frag.GetDatagramSize());
 printf("full decompress=%d header_length=%u offset=%u remaining=%u\n",e,message->GetLength(),message->GetOffset(),frame.GetLength());
 message->Free();
 ot::MeshForwarder::RxInfo rx(instance);rx.mFrameData.Init(bytes+4,length-4);rx.mMacAddrs=addresses;
 ot::Message *full=NULL;
 e=instance.Get<ot::MeshForwarder>().FrameToMessage(rx,frag.GetDatagramSize(),full);
 printf("FrameToMessage=%d length=%u offset=%u\n",e,full?full->GetLength():0,full?full->GetOffset():0);
 if(full)full->Free();
 ot::MeshForwarder::RxInfo incoming(instance);incoming.mFrameData.Init(bytes,length);incoming.mMacAddrs=addresses;
 incoming.mLinkInfo.Clear();incoming.mLinkInfo.mLinkSecurity=true;
 instance.Get<ot::MeshForwarder>().HandleFragment(incoming);
 auto *queued=instance.Get<ot::MeshForwarder>().mReassemblyList.GetHead();
 printf("HandleFragment queued=%u length=%u offset=%u\n",queued!=NULL,queued?queued->GetLength():0,queued?queued->GetOffset():0);
 return queued?0:1;
}
