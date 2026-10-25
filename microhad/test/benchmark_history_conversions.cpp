#include "sensor_history.h"
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <unistd.h>
int main(){char folder[]="/tmp/history-conversion-XXXXXX";if(!mkdtemp(folder))return 1;std::string path=std::string(folder)+"/values";double cost=0;{
SensorHistory history(path);if(!history.appendValue("synthetic",SensorHistory::Temperature,"21.125","celsius",1700000000))return 2;
std::clock_t start=std::clock();for(int i=0;i<100000;++i){
#ifdef TYPED_HISTORY
if(!history.appendNumeric("synthetic",SensorHistory::Temperature,21.125,"celsius",1700000000))return 3;
#else
std::ostringstream text;text<<std::setprecision(15)<<21.125;if(!history.appendValue("synthetic",SensorHistory::Temperature,text.str(),"celsius",1700000000))return 3;
#endif
}cost=1e6*double(std::clock()-start)/CLOCKS_PER_SEC/100000;
}unlink((path+".values").c_str());rmdir(folder);std::printf("%.6f\n",cost);}
