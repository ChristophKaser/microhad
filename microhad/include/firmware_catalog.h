#ifndef MICROHAD_FIRMWARE_CATALOG_H
#define MICROHAD_FIRMWARE_CATALOG_H
#include <string>
void configureFirmwareCatalogs(const std::string& path, const std::string& caBundle);
std::string firmwareCatalogsJson();
bool fetchFirmwareCatalog(const std::string& id, std::string* body, std::string* error);
bool firmwareCatalogDestination(const std::string& configuredUrl, const std::string& candidate);
#endif
