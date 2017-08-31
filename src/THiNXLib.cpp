#include "THiNXLib.h"

extern "C" {
  #include "user_interface.h"
  #include "thinx.h"
}

THiNX::THiNX() {
  // We could init from SPIFFS directly but it is initially empty anyway
  // and otherwise it could cause a lot of distraction.
  available_update_url = "";
}

// Should use THINX_API_KEY from thinx.h; if __apikey is not defined (NULL); resets once-token

THiNX::THiNX(const char * __apikey) {

  thx_udid[64] = {0};
  status = WL_IDLE_STATUS;
  shouldSaveConfig = false;
  connected = false;
  once = true;
  mqtt_client = NULL;
  thx_wifi_client = new WiFiClient();

  // Use __apikey only if set; may be NULL
  if (__apikey) {
    thinx_api_key = strdup(__apikey);
  } else {
    thinx_api_key = strdup("");
  }

  // memory may be optimized later, this should pre-allocate enough except for available_update_url which may be very long
  thinx_udid = strdup("");
  app_version = strdup("");
  available_update_url = strdup("");
  thinx_cloud_url = strdup("");
  thinx_commit_id = strdup("");
  thinx_firmware_version_short = strdup("");
  thinx_firmware_version = strdup("");
  thinx_mqtt_url = strdup("");
  thinx_version_id = strdup("");
  thinx_owner = strdup("");

  // Read constants and possibly stored UDID/API Key
  if (once == true) {
    once = false;
    import_build_time_constants();
  } else  {
    Serial.println("*TH: Already initialized...");
    return;
  }

  if (thinx_api_key) {
    Serial.println("*TH: Init with AK...");
  } else {
    Serial.println("*TH: Init without AK (captive portal)...");
  }

  initWithAPIKey(thinx_api_key);
}

// Designated initializer
void THiNX::initWithAPIKey(const char * __apikey) {

  if (!fsck()) {
    Serial.println("* TH: Filesystem check failed, disabling THiNX."); Serial.flush();
    return;
  }

  bool done = restore_device_info();

  // override from code only if there's no saved API key yet
  if (strlen(thinx_api_key) < 4) {
    // override from code only if override is defined
    if (strlen(__apikey) > 1) {
      Serial.println("*TH: Assigning...");
      thinx_api_key = strdup(__apikey);
    }
  }

  Serial.println("*TH: Connecting...");
  connected = connect();

  // In case device has API Key, it must check-in now:
  if (connected) {
    Serial.println("*TH: Connected to WiFi...");
    Serial.println("*TH: Checking in..."); Serial.flush();
    if (thinx_api_key) {
      checkin();
      mqtt_result = start_mqtt(); // requires valid udid and api_keys, and allocated WiFiClient.
      if (mqtt_result == true) {
        Serial.println("*TH: Starting MQTT...");
      } else {
        Serial.println("*TH: MQTT delayed...");
      }
    } else {
      Serial.println("Skipping checkin, no API Key...");
    }
  } else {
    Serial.println("*TH: Connection to WiFi failed...");
    return;
  }

#ifdef __DEBUG__
  // Serial.println("[update] Trying direct update...");
  // update_and_reboot("/bin/test/firmware.elf");
#endif
}

/*
 * Connection
 */
bool THiNX::connect() {
   bool _connected = false;
   manager = new EAVManager();
   EAVManagerParameter *api_key_param = new EAVManagerParameter("apikey", "API Key", thinx_api_key, 64);
   manager->addParameter(api_key_param);
   manager->setTimeout(10000);
   manager->setDebugOutput(false);
   Serial.println("*TH: AutoConnect with AP Mode (no password):");
   Serial.setDebugOutput(false);
   _connected = manager->autoConnect("AP-THiNX");
   Serial.println("*TH: AutoConnect loop..."); Serial.flush();
   while ( !_connected ) {
     Serial.println("failed to connect and hit timeout"); Serial.flush();
     yield();
     _connected = manager->autoConnect("AP-THiNX");
     if (_connected == true) {
       Serial.print("*TH: connect(): connected, exiting..."); Serial.flush();
     }
   }
   return _connected;
 }

 void THiNX::checkin() {
   Serial.println("*TH: Starting API checkin...");
   if(!connected) {
     Serial.println("*TH: Cannot checkin while not connected, exiting.");
   } else {
     String body = checkin_body();
     senddata(body);
   }
 }

 String THiNX::checkin_body() {

   Serial.println("*TH: Building request...");

   bool done = restore_device_info();

   JsonObject& root = jsonBuffer.createObject();
   root["mac"] = thinx_mac();
   root["firmware"] = String(thinx_firmware_version);
   root["version"] = String(thinx_firmware_version_short);
   root["commit"] = String(thinx_commit_id);
   root["owner"] = String(thinx_owner);
   root["alias"] = String(thinx_alias);

   if (strlen(thinx_udid) > 4) {
     root["udid"] = thinx_udid;
   }

   root["platform"] = String(THINX_PLATFORM);

   Serial.println("*TH: Wrapping request...");

   JsonObject& wrapper = wrapperBuffer.createObject();
   wrapper["registration"] = root;

 #ifdef __DEBUG_JSON__
   wrapper.printTo(Serial);
   Serial.println();
 #endif

   String body;
   wrapper.printTo(body);
   return body;
 }

void THiNX::senddata(String body) {
  char shorthost[256] = {0};
  sprintf(shorthost, "%s", thinx_cloud_url);
  String payload = "";
  if (thx_wifi_client->connect(shorthost, 7442)) {
    // Standard public THiNX-Client device registration request
    // (same request can be called in order to add matching mobile application push token)
    thx_wifi_client->println("POST /device/register HTTP/1.1");
    thx_wifi_client->println("Host: thinx.cloud");
    thx_wifi_client->print("Authentication: "); thx_wifi_client->println(thx_api_key);
    thx_wifi_client->println("Accept: application/json"); // application/json
    thx_wifi_client->println("Origin: device");
    thx_wifi_client->println("Content-Type: application/json");
    thx_wifi_client->println("User-Agent: THiNX-Client");
    thx_wifi_client->print("Content-Length: ");
    thx_wifi_client->println(body.length());
    thx_wifi_client->println();
    Serial.println("Headers set...");
    thx_wifi_client->println(body);
    Serial.println("Body sent...");

    long interval = 10000;
    unsigned long currentMillis = millis(), previousMillis = millis();

    while(!thx_wifi_client->available()){
      yield();
      if( (currentMillis - previousMillis) > interval ){
        Serial.println("Response Timeout. TODO: Should retry later.");
        thx_wifi_client->stop();
        return;
      }
      currentMillis = millis();
    }

    while ( thx_wifi_client->connected() ) {
      yield();
      if ( thx_wifi_client->available() ) {
        char str = thx_wifi_client->read();
        payload = payload + String(str);
      }
    }

    thx_wifi_client->stop();
    parse(payload);

  } else {
    Serial.println("*TH: API connection failed.");
    return;
  }
}

/*
 * Response Parser
 */

void THiNX::parse(String body) {

  // TODO: Should parse response only for this device_id (which must be internal and not a mac)

  payload_type ptype = Unknown;

  int startIndex = 0;
  int endIndex = payload.length();

  int reg_index = payload.indexOf("{\"registration\"");
  int upd_index = payload.indexOf("{\"update\"");
  int not_index = payload.indexOf("{\"notification\"");

  if (upd_index > startIndex) {
    startIndex = upd_index;
    ptype = UPDATE;
  }

  if (reg_index > startIndex) {
    startIndex = reg_index;
    endIndex = payload.indexOf("}}") + 2;
    ptype = REGISTRATION;
  }

  if (not_index > startIndex) {
    startIndex = not_index;
    endIndex = payload.indexOf("}}") + 2; // is this still needed?
    ptype = NOTIFICATION;
  }

  String body = payload.substring(startIndex, endIndex);

#ifdef __DEBUG__
    Serial.print("*TH: Parsing response: '");
    Serial.print(body);
    Serial.println("'");
#endif

  JsonObject& root = jsonBuffer.parseObject(body.c_str());

  if ( !root.success() ) {
  Serial.println("Failed parsing root node.");
    return;
  }

  switch (ptype) {

    case UPDATE: {

      JsonObject& update = root["update"];
      Serial.println("TODO: Parse update payload...");

      // Parse update (work in progress)
      String mac = update["mac"];
      Serial.println(String("mac: ") + mac);

      if (mac != thinx_mac()) {
        Serial.println("*TH: Warning: firmware is dedicated to device with different MAC.");
      }

      // Check current firmware based on commit id and store Updated state...
      String commit = update["commit"];
      Serial.println(String("commit: ") + commit);

      // Check current firmware based on version and store Updated state...
      String version = update["version"];
      Serial.println(String("version: ") + version);

      if ((commit == thinx_commit_id) && (version == thinx_version_id)) {
        if (strlen(available_update_url) > 5) {
          Serial.println("*TH: firmware has same commit_id as current and update availability is stored. Firmware has been installed.");
          available_update_url = "";
          save_device_info();
          notify_on_successful_update();
          return;
        } else {
          Serial.println("*TH: Info: firmware has same commit_id as current and no update is available.");
        }
      }

      // In case automatic updates are disabled,
      // we must ask user to commence firmware update.
      if (THINX_AUTO_UPDATE == false) {
        if (mqtt_client) {
          mqtt_client->publish(
            thinx_mqtt_channel().c_str(),
            thx_update_question.c_str()
          );
        }

      } else {

        Serial.println("Starting update...");

        // FROM LUA: update variants
        // local files = payload['files']
        // local ott   = payload['ott']
        // local url   = payload['url']
        // local type  = payload['type']

        String url = update["url"]; // may be OTT URL
        available_update_url = url.c_str();
        save_device_info();
        if (url) {
          Serial.println("*TH: Force update URL must not contain HTTP!!! :" + url);
          url.replace("http://", "");
          // TODO: must not contain HTTP, extend with http://thinx.cloud/"
          // TODO: Replace thinx.cloud with thinx.local in case proxy is available
          update_and_reboot(url);
        }
        return;
      }

    } break;

    case NOTIFICATION: {

      // Currently, this is used for update only, can be extended with request_category or similar.
      JsonObject& notification = root["notification"];

      if ( !notification.success() ) {
        Serial.println("Failed parsing notification node.");
        return;
      }

      String type = notification["response_type"];
      if ((type == "bool") || (type == "boolean")) {
        bool response = notification["response"];
        if (response == true) {
          Serial.println("User allowed update using boolean.");
          if (strlen(available_update_url) > 4) {
            update_and_reboot(available_update_url);
          }
        } else {
          Serial.println("User denied update using boolean.");
        }
      }

      if ((type == "string") || (type == "String")) {
        String response = notification["response"];
        if (response == "yes") {
          Serial.println("User allowed update using string.");
          if (strlen(available_update_url) > 4) {
            update_and_reboot(available_update_url);
          }
        } else if (response == "no") {
          Serial.println("User denied update using string.");
        }
      }

    } break;

    case REGISTRATION: {

      JsonObject& registration = root["registration"];

      if ( !registration.success() ) {
        Serial.println("Failed parsing registration node.");
        return;
      }

      bool success = registration["success"];
      String status = registration["status"];

      if (status == "OK") {

        String alias = registration["alias"];
        Serial.print("Reading alias: ");
        Serial.print(alias);
        if ( alias.length() > 0 ) {
          Serial.println(String("assigning alias: ") + alias);
          thinx_alias = strdup(alias.c_str());
        }

        String owner = registration["owner"];
        Serial.println("Reading owner: ");
        if ( owner.length() > 0 ) {
          Serial.println(String("assigning owner: ") + owner);
          thinx_owner = strdup(owner.c_str());
        }

        Serial.println("Reading udid: ");
        String udid = registration["udid"];
        if ( udid.length() > 4 ) {
          Serial.println(String("assigning udid: ") + udid);
          thinx_udid = strdup(udid.c_str());
        }

        // Check current firmware based on commit id and store Updated state...
        String commit = registration["commit"];
        Serial.println(String("commit: ") + commit);

        // Check current firmware based on version and store Updated state...
        String version = registration["version"];
        Serial.println(String("version: ") + version);

        if ((commit == thinx_commit_id) && (version == thinx_version_id)) {
          if (strlen(available_update_url) > 4) {
            Serial.println("*TH: firmware has same commit_id as current and update availability is stored. Firmware has been installed.");
            available_update_url = "";
            save_device_info();
            notify_on_successful_update();
            return;
          } else {
            Serial.println("*TH: Info: firmware has same commit_id as current and no update is available.");
          }
        }

        save_device_info();

      } else if (status == "FIRMWARE_UPDATE") {

        String mac = registration["mac"];
        Serial.println(String("mac: ") + mac);
        // TODO: must be current or 'ANY'

        String commit = registration["commit"];
        Serial.println(String("commit: ") + commit);

        // should not be same except for forced update
        if (commit == thinx_commit_id) {
          Serial.println("*TH: Warning: new firmware has same commit_id as current.");
        }

        String version = registration["version"];
        Serial.println(String("version: ") + version);

        Serial.println("Starting update...");

        String url = registration["url"];
        if (url) {
          Serial.println("*TH: Running update with URL that should not contain http! :" + url);
          url.replace("http://", "");
          update_and_reboot(url);
        }
      }

      } break;

    default:
      Serial.println("Nothing to do...");
      break;
  }

}

/*
 * MQTT
 */

String THiNX::thinx_mqtt_channel() {
 return String("/") + thinx_owner + "/" + thinx_udid;
}

String THiNX::thinx_mqtt_status_channel() {
 return String("/") + thinx_owner + "/" + thinx_udid + "/status";
}

String THiNX::thinx_mac() {
 byte mac[] = {
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00
 };
 WiFi.macAddress(mac);
 char macString[16] = {0};
 sprintf(macString, "5CCF7F%6X", ESP.getChipId()); // ESP8266 only!
 return String(macString);
}


void THiNX::publish() {
  if (!connected) return;
  if (mqtt_client == NULL) return;
  if (strlen(thinx_udid) < 4) return;
  bool done = restore_device_info(); // thinx_mqtt_status_channel() requires owner and uuid
  String channel = thinx_mqtt_status_channel();
  if (mqtt_client->connected()) {
    Serial.println("*TH: MQTT connected...");
    mqtt_client->publish(channel.c_str(), thx_connected_response.c_str());
    mqtt_client->loop();
  } else {
    Serial.println("*TH: MQTT not connected, reconnecting...");
    mqtt_result = start_mqtt();
    if (mqtt_result && mqtt_client->connected()) {
      mqtt_client->publish(channel.c_str(), thx_connected_response.c_str());
      Serial.println("*TH: MQTT reconnected, published default message.");
    } else {
      Serial.println("*TH: MQTT Reconnect failed...");
    }
  }
}

void THiNX::notify_on_successful_update() {
  if (mqtt_client) {
    String success = "{ title: \"Update Successful\", body: \"The device has been successfully updated.\", type: \"success\" }";
    mqtt_client->publish(
      thinx_mqtt_status_channel().c_str(),
      success.c_str()
    );
  } else {
    Serial.println("Device updated but MQTT not active to notify. TODO: Store.");
  }
}

bool THiNX::start_mqtt() {

  if (strlen(thinx_udid) < 4) {
    return false;
  }

  Serial.print("*TH: UDID: ");
  Serial.println(thinx_udid);

  Serial.print("*TH: Contacting MQTT server ");
  Serial.println(thinx_mqtt_url);

  //PubSubClient mqtt_client(thx_wifi_client, thinx_mqtt_url.c_str());
  Serial.print("*TH: Starting client");
  if (mqtt_client == NULL) {
    mqtt_client = new PubSubClient(*thx_wifi_client, thinx_mqtt_url);
  }

  Serial.print(" on port ");
  Serial.println(thinx_mqtt_port);

  last_mqtt_reconnect = 0;

  String channel = thinx_mqtt_channel();
  Serial.println("*TH: Connecting to MQTT...");


  Serial.print("*TH: AK: ");
  Serial.println(thinx_api_key);
  Serial.print("*TH: DCH: ");
  Serial.println(channel);

  String mac = thinx_mac();

  const char* id = mac.c_str();
  const char* user = thinx_udid;
  const char* pass = thinx_api_key;
  const char* willTopic = thinx_mqtt_channel().c_str();
  int willQos = 0;
  bool willRetain = false;

  if (mqtt_client->connect(MQTT::Connect(id)
                .set_will(willTopic, thx_disconnected_response.c_str())
                .set_auth(user, pass)
                .set_keepalive(30)
              )) {

      mqtt_client->set_callback([this](const MQTT::Publish &pub){

        Serial.println("*TH: MQTT callback...");
        Serial.print(pub.topic());
        Serial.print(" => ");

        if (pub.has_stream()) {
          Serial.println("*TH: MQTT Type: Stream...");
          Serial.setDebugOutput(true);
          uint32_t startTime = millis();
          uint32_t size = pub.payload_len();
          if ( ESP.updateSketch(*pub.payload_stream(), size, true, false) ) {

            // Notify on reboot for update
            mqtt_client->publish(
              thinx_mqtt_status_channel().c_str(),
              thx_reboot_response.c_str()
            );
            mqtt_client->disconnect();
            pub.payload_stream()->stop();
            Serial.printf("Update Success: %u\nRebooting...\n", millis() - startTime);
            ESP.restart();
          }

          Serial.println("stop.");

        } else {

          Serial.println("*TH: MQTT Type: String or JSON...");
          String payload = pub.payload_string();
          Serial.println(payload);
          parse(payload);

        }
    }); // end-of-callback

    Serial.print("*TH: MQTT Subscribing device channel: ");
    Serial.println(thinx_mqtt_channel());

    if (mqtt_client->subscribe(thinx_mqtt_channel().c_str())) {
      Serial.print("*TH: ");
      Serial.print(thinx_mqtt_channel());
      Serial.println(" successfully subscribed.");

      // Publish status on status channel
      mqtt_client->publish(
        thinx_mqtt_status_channel().c_str(),
        thx_connected_response.c_str()
      );

      // Publish registration on status channel to request possible update
      mqtt_client->publish(
        thinx_mqtt_status_channel().c_str(),
        checkin_body().c_str()
      );

      return true;
  } else {
    Serial.println("*TH: MQTT Not connected.");
    return false;
  }
}}

//
// EAVManager Setup Callbacks
//

void THiNX::saveConfigCallback() {
  Serial.println("Saveing configuration...");
  strcpy(thx_api_key, api_key_param->getValue());
  if (strlen(thx_api_key) > 4) {
    thinx_api_key = thx_api_key;
    Serial.print("Saving thinx_api_key: ");
    Serial.println(thinx_api_key);
    save_device_info();
  }
}

/*
 * Device Info
 */

bool THiNX::restore_device_info() {
   File f = SPIFFS.open("/thx.cfg", "r");
   if (!f) {
       Serial.println("*TH: No remote configuration found so far...");
       return false;
   }
   String data = f.readStringUntil('\n');
   JsonObject& config = jsonBuffer.parseObject(data.c_str());
   if (!config.success()) {
     Serial.println("*TH: parseObject() failed");
     return false;
   } else {
     const char* saved_alias = config["alias"];
     if (strlen(saved_alias) > 1) {
       thinx_alias = strdup(saved_alias);
     }
     const char* saved_owner = config["owner"];
     if (strlen(saved_owner) > 4) {
       thinx_owner = strdup(saved_owner);
     }
     const char* saved_apikey = config["apikey"];
     if (strlen(saved_apikey) > 8) {
      thinx_api_key = strdup(saved_apikey);
      sprintf(thx_api_key, "%s", saved_apikey); // 40 max; copy; should deprecate
     }
     const char* saved_update = config["update"];
     if (strlen(saved_update) > 4) {
       available_update_url = saved_update;
     }
     const char* saved_udid = config["udid"];
     Serial.print("*TH: Saved udid: "); Serial.println(saved_udid);
     if ((strlen(saved_udid) > 4)) {
      thinx_udid = strdup(saved_udid);
    } else {
      Serial.println("Using THINX_UDID");
      Serial.println(THINX_UDID);
      thinx_udid = strdup(THINX_UDID);
    }
    sprintf(thx_udid, "%s", thinx_udid); // 40 max
    f.close();
   }
   return true;
 }

 /* Stores mutable device data (alias, owner) retrieved from API */
 void THiNX::save_device_info()
 {
   const char *config = deviceInfo().c_str();

   File f = SPIFFS.open("/thx.cfg", "w");
   if (f) {
     Serial.print("*TH: saving configuration...");
     f.close();
   } else {
     Serial.println("*TH: Cannot save configuration, formatting SPIFFS...");
     SPIFFS.format();
     Serial.println("*TH: Trying to save again...");
     f = SPIFFS.open("/thx.cfg", "w");
     if (f) {
       save_device_info();
       f.close();
     } else {
       Serial.println("*TH: Retry failed...");
     }
   }

   Serial.println("*TH: save_device_info() completed.");
   SPIFFS.end();
   Serial.println("*TH: SPIFFS.end();");

   bool done = restore_device_info();
 }

String THiNX::deviceInfo() {
  Serial.println("*TH: building device info:");
  JsonObject& root = jsonBuffer.createObject();
  root["alias"] = thinx_alias;
  Serial.print("*TH: thinx_alias: ");
  Serial.println(thinx_alias);

  root["owner"] = thinx_owner;
  Serial.print("*TH: thinx_owner: ");
  Serial.println(thinx_owner);

  root["apikey"] = thx_api_key;
  Serial.print("*TH: thx_api_key: ");
  Serial.println(thx_api_key);

  root["udid"] = thinx_udid;
  Serial.print("*TH: thinx_udid: ");
  Serial.println(thinx_udid);

  root["update"] = available_update_url;
  Serial.print("*TH: available_update_url: ");
  Serial.println(available_update_url);

  String jsonString;
  root.printTo(jsonString);

  return jsonString;
}


/*
 * Updates
 */

// update_file(name, data)
// update_from_url(name, url)

void THiNX::update_and_reboot(String url) {

#ifdef __DEBUG__
  Serial.println("[update] Starting update...");
#endif

// #define __USE_ESP__
#ifdef __USE_ESP__
  uint32_t size = pub.payload_len();
  if (ESP.updateSketch(*pub.payload_stream(), size, true, false)) {
    Serial.println("Clearing retained message.");
    mqtt_client->publish(MQTT::Publish(pub.topic(), "").set_retain());
    mqtt_client->disconnect();

    Serial.printf("Update Success: %u\nRebooting...\n", millis() - startTime);

    // Notify on reboot for update
    if (mqtt_client) {
      mqtt_client->publish(
        thinx_mqtt_status_channel().c_str(),
        thx_reboot_response.c_str()
      );
      mqtt_client->disconnect();
    }

    ESP.restart();
  }
#else

  t_httpUpdate_return ret = ESPhttpUpdate.update("thinx.cloud", 80, url.c_str());

  switch(ret) {
    case HTTP_UPDATE_FAILED:
    Serial.println("[update] Update failed.");
    break;
    case HTTP_UPDATE_NO_UPDATES:
    Serial.println("[update] Update no Update.");
    break;
    case HTTP_UPDATE_OK:
    Serial.println("[update] Update ok."); // may not called we reboot the ESP
    break;
  }

  if (ret != HTTP_UPDATE_OK) {
    Serial.println("[update] WiFi connected, trying advanced update...");
    Serial.println("[update] TODO: Rewrite to secure binary provider on the API side!");
    ret = ESPhttpUpdate.update("images.thinx.cloud", 80, "ota.php", "5ccf7fee90e0");
    switch(ret) {
      case HTTP_UPDATE_FAILED:
      Serial.println("[update] Update failed.");
      break;
      case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[update] Update no Update.");
      break;
      case HTTP_UPDATE_OK:
      Serial.println("[update] Update ok."); // may not called we reboot the ESP
      break;
    }
  }
#endif
}

/* Imports all required build-time values from thinx.h */
void THiNX::import_build_time_constants() {
  thinx_api_key = strdup(THINX_API_KEY);
  thinx_udid = strdup(THINX_UDID);
  thinx_commit_id = strdup(THINX_COMMIT_ID);
  thinx_mqtt_url = strdup(THINX_MQTT_URL);
  thinx_cloud_url = strdup(THINX_CLOUD_URL);
  thinx_alias = strdup(THINX_ALIAS);
  thinx_owner = strdup(THINX_OWNER);
  thinx_mqtt_port = THINX_MQTT_PORT;
  thinx_api_port = THINX_API_PORT;
  thinx_auto_update = THINX_AUTO_UPDATE;
  thinx_forced_update = THINX_FORCED_UPDATE;
  thinx_firmware_version = strdup(THINX_FIRMWARE_VERSION);
  thinx_firmware_version_short = strdup(THINX_FIRMWARE_VERSION_SHORT);
  app_version = strdup(THINX_APP_VERSION);
#ifdef __DEBUG__
  Serial.println("*TH: Imported build-time constants...");
#endif
}

bool THiNX::fsck() {
    realSize = String(ESP.getFlashChipRealSize());
    ideSize = String(ESP.getFlashChipSize());
    flashCorrectlyConfigured = realSize.equals(ideSize);
    fileSystemReady = false;
    Serial.print("* TH: SPIFFS real size = ");
    Serial.println(realSize); Serial.flush();
    Serial.print("* TH: SPIFFS IDE size = ");
    Serial.println(ideSize); Serial.flush();
    Serial.println("* TH: Running file-system check...");
    if(flashCorrectlyConfigured) {
      Serial.println("* TH: Starting SPIFFS..."); Serial.flush();
      fileSystemReady = SPIFFS.begin();
      if (!fileSystemReady) {
        Serial.println("* TH: Formatting SPIFFS..."); Serial.flush();
        fileSystemReady = SPIFFS.format();;
        Serial.println("* TH: Format complete, rebooting..."); Serial.flush();
        ESP.reset();
        return false;
      }
      Serial.println("* TH: SPIFFS Initialization completed."); Serial.flush();
    }  else {
      Serial.println("flash incorrectly configured, SPIFFS cannot start, IDE size: " + ideSize + ", real size: " + realSize); Serial.flush();
    }
    return fileSystemReady;
}

/*
 * Core loop
 */

void THiNX::loop() {
  if (connected) {
    // uint32_t memfree = system_get_free_heap_size(); Serial.print("PRE-PUBLISH memfree: "); Serial.println(memfree);
    publish();
    //Serial.print("POST-PUBLISH memfree: "); memfree = system_get_free_heap_size(); Serial.println(memfree);
  }
  if (mqtt_client != NULL) {
    mqtt_client->loop();
  }
}
