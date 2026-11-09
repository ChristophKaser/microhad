// Exercise the linked OpenThread radio state machine with a synthetic clock.
#include "lib/spinel/radio_spinel.hpp"
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"Failed at line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
static uint64_t nowUs;
static unsigned completed;
extern "C" uint64_t __wrap_otPlatTimeGet(void) { return nowUs; }
extern "C" void otPlatReset(otInstance *) { std::abort(); }
static void transmitDone(otInstance *,otRadioFrame *,otRadioFrame *,otError error) {
    CHECK(error==OT_ERROR_NO_ACK);completed++;
}
static void pending(ot::Spinel::RadioSpinel &radio,otRadioFrame &frame) {
    radio.mState=ot::Spinel::RadioSpinel::kStateTransmitting;
    radio.mTxRadioTid=1;
    radio.mTransmitFrame=&frame;
    radio.mTxRadioEndUs=radio.kTxWaitUs;
    radio.mCallbacks.mTransmitDone=transmitDone;
}
int main(void) {
    alarm(5);
    otRadioFrame frame={};
    ot::Spinel::RadioSpinel radio;
    CHECK(radio.kTxWaitUs==15000000);
    pending(radio,frame);
    nowUs=14000000;
    radio.Process(nullptr);
    CHECK(completed==0);
    // A real Spinel completion with NoAck still completes the radio exchange.
    const uint8_t response[]={0x81,SPINEL_CMD_PROP_VALUE_IS,SPINEL_PROP_LAST_STATUS,SPINEL_STATUS_NO_ACK,0,0};
    radio.HandleResponse(response,sizeof(response));
    radio.Process(nullptr);
    CHECK(completed==1);
    pid_t child=fork();CHECK(child>=0);
    if(child==0) {
        pending(radio,frame);nowUs=14999999;radio.Process(nullptr);
        nowUs=15000000;radio.Process(nullptr);
        _exit(99);
    }
    int status;CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==6);
    std::puts("Radio timeout: delayed completion at 14 seconds accepted; missing completion exits at 15 seconds");
}
