#ifndef MICROHAD_SCRIPT_BENCHMARK_H
#define MICROHAD_SCRIPT_BENCHMARK_H

#include <string>

/* This deliberately runs before GatewayConfig and every gateway worker. */
bool runScriptBenchmark(const std::string& runtime, const std::string& filename,
                        unsigned long runs, std::string* json, std::string* error);

#endif
