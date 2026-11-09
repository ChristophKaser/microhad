#ifndef MICROHAD_FIRMWARE_CATALOG_H
#define MICROHAD_FIRMWARE_CATALOG_H
#include <string>
void configureFirmwareCatalogs(const std::string& path, const std::string& caBundle);
std::string firmwareCatalogsJson();
class FirmwareCatalogSink {
public:
    virtual ~FirmwareCatalogSink() {}
    virtual bool begin() = 0;
    virtual bool write(const char* data, unsigned long size) = 0;
};
bool streamFirmwareCatalog(const std::string& id, FirmwareCatalogSink* sink, bool* started, std::string* error);
bool firmwareCatalogDestination(const std::string& configuredUrl, const std::string& candidate);
#endif
