#!/usr/bin/env python3
"""Execute the patched firmware RX functions with mocked DMA and GPIO flow control."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
PREAMBLE=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int UARTDRV_Handle_t;
typedef unsigned UARTDRV_Count_t;
typedef int Ecode_t;
#define OT_UNUSED_VARIABLE(x) (void)(x)
#define CORE_ATOMIC_SECTION(...) do { __VA_ARGS__ } while(0);
#define ECODE_EMDRV_UARTDRV_OK 0
#define uartdrvFlowControlOn 1
#define uartdrvFlowControlOff 0
#define uartdrvFlowControlAuto 2
#define sl_uartdrv_usart_vcom_handle 1
#define SL_UARTDRV_USART_VCOM_PERIPHERAL 1
#define USART_IF_RXOF 1
static unsigned flags,signals,queued,flow_pauses,flow_resumes;
static int queue_status, self_state;
static uint8_t *dma;
static unsigned dma_count;
static uint8_t output[65536];static unsigned output_len;
static bool inject;
static uint32_t USART_IntGet(int h){(void)h;return flags;}
static void USART_IntClear(int h,unsigned f){(void)h;flags&=~f;}
static int UARTDRV_FlowControlSet(int h,int s){(void)h;if(s==0){flow_pauses++;self_state=0;}else if(s==2){flow_resumes++;}else {assert(s==1);self_state=1;}return 0;}
static int UARTDRV_Receive(int h,uint8_t *data,unsigned n,void (*cb)(int,int,uint8_t *,unsigned)){
 (void)h;(void)data;(void)cb;assert(n==128);queued++;return queue_status;
}
static void UARTDRV_GetReceiveStatus(int h,uint8_t **p,unsigned *count,unsigned *remaining){
 (void)h;*p=dma;*count=dma_count;*remaining=128-dma_count;
}
static void otSysEventSignalPending(void){signals++;}
static void otPlatUartReceived(const uint8_t *,unsigned);
'''
TEST=r'''
static void otPlatUartReceived(const uint8_t *bytes,unsigned n){
 assert(output_len+n<=sizeof(output));memcpy(output+output_len,bytes,n);output_len+=n;
 if(inject){inject=false;receiveDone(1,0,sReceiveBuffer2,128);}
}
static void reset(void){
 memset(&sReceiveFifo,0,sizeof(sReceiveFifo));sRxDroppedBytes=sRxTransferErrors=sRxQueueErrors=sRxOverruns=sRxCountErrors=sRxPauseCount=0;
 sRxHighWater=0;flags=signals=queued=flow_pauses=flow_resumes=0;queue_status=0;
 output_len=0;dma=sReceiveBuffer1;dma_count=0;inject=false;resetReceiveState();
}
int main(void){unsigned i;char stats[128];
 reset();assert(kReceiveFifoSize==512 && RECEIVE_BUFFER_SIZE==128);
 // Pause at 255 queued bytes; resume only when at most 128 remain.
 sReceiveFifo.mTail=254;updateReceiveFlowControl();assert(!sRxRestrained);
 sReceiveFifo.mTail=255;updateReceiveFlowControl();assert(sRxRestrained && self_state==0);
 sReceiveFifo.mHead=126;updateReceiveFlowControl();assert(sRxRestrained);
 sReceiveFifo.mHead=127;updateReceiveFlowControl();assert(!sRxRestrained && self_state==1);
 reset();
 // Full DMA progress must not be copied twice at completion.
 for(i=0;i<128;i++)sReceiveBuffer1[i]=i;
 dma_count=128;processReceive();assert(lastCount==128 && output_len==128);
 receiveDone(1,0,sReceiveBuffer1,128);assert(lastCount==0 && sRxDroppedBytes==0);
 dma=sReceiveBuffer2;dma_count=1;sReceiveBuffer2[0]=77;processReceive();assert(output_len==129 && output[128]==77);
 // A split write at the ring end preserves both data and index fields.
 reset();sReceiveFifo.mHead=400;sReceiveFifo.mTail=488;
 for(i=0;i<64;i++)sReceiveBuffer1[i]=i+1;
 assert(copyReceiveBytes(sReceiveBuffer1,64)==64);assert(sReceiveFifo.mHead==400&&sReceiveFifo.mTail==40);
 for(i=0;i<64;i++)assert(sReceiveFifo.mBuffer[(488+i)%512]==i+1);
 // Deliberately ignore CTS to force full-buffer behavior: count every drop.
 reset();memset(sReceiveBuffer1,23,128);
 for(i=0;i<4;i++)receiveDone(1,0,sReceiveBuffer1,128);
 assert(receiveUsed()==511 && sRxDroppedBytes==1 && lastCount==0);
 assert(flow_pauses==1 && sRxRestrained && self_state==0);
 processReceive();assert(output_len==511 && !sRxRestrained && sRxHighWater==511 && self_state==1);
 for(i=0;i<output_len;i++)assert(output[i]==23);
 receiveDone(1,0,sReceiveBuffer1,128);processReceive();assert(output_len==639 && sRxDroppedBytes==1);
 // Keep uncopied partial DMA bytes in place until the next foreground pass.
 reset();uint8_t fill[508];memset(fill,42,sizeof(fill));assert(copyReceiveBytes(fill,sizeof(fill))==508);
 for(i=0;i<10;i++)sReceiveBuffer1[i]=100+i;
 dma_count=10;processReceive();assert(lastCount==3 && output_len==511 && signals>0);
 processReceive();assert(lastCount==10 && output_len==518 && sRxDroppedBytes==0);
 for(i=0;i<10;i++)assert(output[508+i]==100+i);
 // A DMA completion during decoder consumption cannot overwrite held bytes.
 reset();memset(sReceiveBuffer1,5,100);memset(sReceiveBuffer2,7,128);
 assert(copyReceiveBytes(sReceiveBuffer1,100)==100);inject=true;processReceive();
 assert(output_len==100);processReceive();assert(output_len==228);
 for(i=0;i<228;i++)assert(output[i]==(i<100?5:7));
 // Keep hardware/queue failures distinct from software FIFO loss.
 reset();flags=USART_IF_RXOF;queue_status=9;receiveDone(1,8,sReceiveBuffer1,128);
 assert(sRxOverruns==1 && sRxTransferErrors==1 && sRxQueueErrors==1 && flags==0);
 efr32UartRxDiagnostics(stats,sizeof(stats));assert(strstr(stats,"drop=0 err=1 queue=1 ovf=1 count=0"));
 transmitDone(1,0,sReceiveBuffer1,0);assert(sTxComplete);
 puts("PASS patched UART: 128-byte DMA, wraparound, full FIFO, RTS pause/resume, deferred bytes, ISR interleaving and error counters");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='uart-rx-test-') as temporary:
        out=Path(temporary)
        project=out/'ot-rcp-multipan.project.mak'
        project.write_text('$(SDK_PATH)/util/third_party/openthread/src/ncp/ncp_hdlc.cpp\n$(SDK_PATH)/protocol/openthread/platform-abstraction/efr32/uartdrv_uart.c\n')
        spec=importlib.util.spec_from_file_location('prepare',ROOT/'firmware/scripts/prepare-hdlc-diagnostics.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        module.prepare(ROOT/'firmware/dependencies/gecko-sdk',out)
        code=(out/'diagnostics/uartdrv_uart.c').read_text()
        block=code[code.index('enum\n{\n    kReceiveFifoSize'):code.index('static void processTransmit(void)\n{')]
        block=block.replace('static void processTransmit(void);','')
        source=out/'test.c';source.write_text(PREAMBLE+block+TEST)
        binary=out/'test'
        subprocess.run(['gcc','-std=gnu99','-Wall','-Wextra','-Werror',str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__=='__main__':main()
