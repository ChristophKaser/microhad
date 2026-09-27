#ifndef MICROHAD_ZIGBEE_FCM_H
#define MICROHAD_ZIGBEE_FCM_H

#include <string>

struct FirebaseServiceAccount {
    std::string projectId;
    std::string clientEmail;
    std::string privateKeyPem;
};

bool parseFirebaseServiceAccount(const std::string& json, FirebaseServiceAccount* account, std::string* error);
std::string base64UrlEncode(const std::string& data);
bool base64UrlDecode(const std::string& value, std::string* decoded);
std::string makeFcmMessage(const std::string& topic, const std::string& title, const std::string& body);
std::string makeFcmTokenMessage(const std::string& token, const std::string& title, const std::string& body);
std::string makeGoogleAccessTokenJwt(const FirebaseServiceAccount& account, long issuedAt, std::string* error);
bool sendFcmNotification(const std::string& serviceAccountPath, const std::string& caBundle,
                         const std::string& topic, const std::string& title, const std::string& body,
                         std::string* error);
bool sendFcmTokenNotification(const std::string& serviceAccountPath, const std::string& caBundle,
                              const std::string& token, const std::string& title, const std::string& body,
                              std::string* error);

#endif
