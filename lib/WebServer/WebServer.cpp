#include <WebServer.h>

static const char INDEX_FILENAME[] = "/index.html";
static const char TEXT_HTML[] = "text/html";
static const char TEXT_PLAIN[] = "text/plain";
static const char APPLICATION_JSON[] = "application/json";

static const char CONTENT_TYPE_HEADER[] = "Content-Type";

WebServer::WebServer(DisplayTemplateDriver& driver, Settings& settings)
  : driver(driver),
    settings(settings),
    server(AsyncWebServer(80))
{ }

void WebServer::begin() {
  on("/variables", HTTP_PUT, handleUpdateVariables());
  on("/variables", HTTP_GET, handleServeFile(VariableDictionary::FILENAME, APPLICATION_JSON));

  onUpload("/templates", HTTP_POST, handleCreateFile(TEMPLATES_DIRECTORY));
  onPattern("/templates/:filename", HTTP_DELETE, handleDeleteTemplate());
  onPattern("/templates/:filename", HTTP_GET, handleShowTemplate());
  onPattern("/templates/:filename", HTTP_PUT, handleUpdateTemplate());
  on("/templates", HTTP_GET, handleListDirectory(TEMPLATES_DIRECTORY));

  onUpload("/bitmaps", HTTP_POST, handleCreateFile(BITMAPS_DIRECTORY));
  onPattern("/bitmaps/:filename", HTTP_DELETE, handleDeleteBitmap());
  onPattern("/bitmaps/:filename", HTTP_GET, handleShowBitmap());
  on("/bitmaps", HTTP_GET, handleListDirectory(BITMAPS_DIRECTORY));

  on("/settings", HTTP_GET, handleListSettings());
  on("/settings", HTTP_PUT, handleUpdateSettings());

  on("/about", HTTP_GET, handleAbout());

  on("/", HTTP_GET, handleServeFile(INDEX_FILENAME, TEXT_HTML));
  onUpload("/index.html", HTTP_POST, handleUpdateFile(INDEX_FILENAME));

  server.begin();
}

ArRequestHandlerFunction WebServer::handleAbout() {
  return [this](AsyncWebServerRequest* request) {
    // Measure before allocating buffers
    uint32_t freeHeap = ESP.getFreeHeap();

    StaticJsonBuffer<150> buffer;
    JsonObject& res = buffer.createObject();

    res["version"] = QUOTE(EPAPER_TEMPLATES_VERSION);
    res["variant"] = QUOTE(FIRMWARE_VARIANT);
    res["free_heap"] = freeHeap;
    res["sdk_version"] = ESP.getSdkVersion();

    String body;
    res.printTo(body);

    request->send(200, APPLICATION_JSON, body);
  };
}

ArRequestHandlerFunction WebServer::sendSuccess() {
  return [this](AsyncWebServerRequest* request) {
    request->send(200, APPLICATION_JSON, "true");
  };
}

ArBodyHandlerFunction WebServer::handleUpdateVariables() {
  return [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    DynamicJsonBuffer buffer;
    JsonObject& vars = buffer.parseObject(data);

    if (! vars.success()) {
      request->send_P(400, TEXT_PLAIN, PSTR("Invalid JSON"));
      return;
    }

    for (JsonObject::iterator itr = vars.begin(); itr != vars.end(); ++itr) {
      driver.updateVariable(itr->key, itr->value);
    }

    request->send_P(200, APPLICATION_JSON, PSTR("true"));
  };
}

ArRequestHandlerFunction WebServer::handleServeFile(
  const char* filename,
  const char* contentType,
  const char* defaultText) {

  return [this, filename, contentType, defaultText](AsyncWebServerRequest* request) {
    if (!serveFile(request, filename, contentType)) {
      if (defaultText) {
        request->send(200, contentType, defaultText);
      } else {
        request->send(404);
      }
    }
  };
}

bool WebServer::serveFile(AsyncWebServerRequest* request, const char* file, const char* contentType) {
  if (SPIFFS.exists(file)) {
    request->send(SPIFFS, file, contentType);
    return true;
  }

  return false;
}

// ---------
// CRUD handlers for bitmaps
// ---------

PatternHandler::TPatternHandlerFn WebServer::handleShowBitmap() {
  return [this](const UrlTokenBindings* bindings, AsyncWebServerRequest* request) {
    if (bindings->hasBinding("filename")) {
      const char* filename = bindings->get("filename");
      String path = String(BITMAPS_DIRECTORY) + "/" + filename;

      request->send(SPIFFS, path, "application/octet-stream");
    } else {
      request->send_P(400, TEXT_PLAIN, PSTR("You must provide a filename"));
    }
  };
}

PatternHandler::TPatternHandlerFn WebServer::handleDeleteBitmap() {
  return [this](const UrlTokenBindings* bindings, AsyncWebServerRequest* request) {
    if (bindings->hasBinding("filename")) {
      const char* filename = bindings->get("filename");
      String path = String(BITMAPS_DIRECTORY) + "/" + filename;

      if (SPIFFS.exists(path)) {
        if (SPIFFS.remove(path)) {
          request->send_P(200, TEXT_PLAIN, PSTR("success"));
        } else {
          request->send_P(500, TEXT_PLAIN, PSTR("Failed to delete file"));
        }
      } else {
        request->send(404, TEXT_PLAIN);
      }
    } else {
      request->send_P(400, TEXT_PLAIN, PSTR("You must provide a filename"));
    }
  };
}

ArRequestHandlerFunction WebServer::handleListDirectory(const char* dirName) {
  return [this, dirName](AsyncWebServerRequest* request) {
    DynamicJsonBuffer buffer;
    JsonArray& responseObj = buffer.createArray();

#if defined(ESP8266)
    Dir dir = SPIFFS.openDir(dirName);

    while (dir.next()) {
      JsonObject& file = buffer.createObject();
      file["name"] = dir.fileName();
      file["size"] = dir.fileSize();
      responseObj.add(file);
    }
#elif defined(ESP32)
    File dir = SPIFFS.open(dirName);

    if (!dir || !dir.isDirectory()) {
      Serial.print(F("Path is not a directory - "));
      Serial.println(dirName);

      request->send_P(500, TEXT_PLAIN, PSTR("Expected path to be a directory, but wasn't"));
      return;
    }

    while (File dirFile = dir.openNextFile()) {
      JsonObject& file = buffer.createObject();

      file["name"] = String(dirFile.name());
      file["size"] = dirFile.size();

      responseObj.add(file);
    }
#endif

    String response;
    responseObj.printTo(response);

    request->send(200, APPLICATION_JSON, response);
  };
}

// ---------
// CRUD handlers for templates
// ---------

PatternHandler::TPatternHandlerFn WebServer::handleShowTemplate() {
  return [this](const UrlTokenBindings* bindings, AsyncWebServerRequest* request) {
    if (bindings->hasBinding("filename")) {
      const char* filename = bindings->get("filename");
      String path = String(TEMPLATES_DIRECTORY) + "/" + filename;
      request->send(SPIFFS, path, APPLICATION_JSON);
    } else {
      request->send_P(400, TEXT_PLAIN, PSTR("You must provide a filename"));
    }
  };
}

PatternHandler::TPatternHandlerBodyFn WebServer::handleUpdateTemplate() {
  return [this](
    const UrlTokenBindings* bindings,
    AsyncWebServerRequest* request,
    uint8_t* data,
    size_t len,
    size_t index,
    size_t total
  ) {
    if (bindings->hasBinding("filename")) {
      const char* filename = bindings->get("filename");
      String path = String(TEMPLATES_DIRECTORY) + "/" + filename;
      handleUpdateJsonFile(path, request, data, len);
    }
  };
}

PatternHandler::TPatternHandlerFn WebServer::handleDeleteTemplate() {
  return [this](const UrlTokenBindings* bindings, AsyncWebServerRequest* request) {
    if (bindings->hasBinding("filename")) {
      const char* filename = bindings->get("filename");
      String path = String(TEMPLATES_DIRECTORY) + "/" + filename;

      if (SPIFFS.exists(path)) {
        if (SPIFFS.remove(path)) {
          request->send_P(200, TEXT_PLAIN, PSTR("success"));
        } else {
          request->send_P(500, TEXT_PLAIN, PSTR("Failed to delete file"));
        }
      } else {
        request->send(404, TEXT_PLAIN);
      }
    } else {
      request->send_P(400, TEXT_PLAIN, PSTR("You must provide a filename"));
    }
  };
}

ArUploadHandlerFunction WebServer::handleCreateFile(const char* filePrefix) {
  return [this, filePrefix](
    AsyncWebServerRequest *request,
    const String& filename,
    size_t index,
    uint8_t *data,
    size_t len,
    bool isFinal
  ) {
    static File updateFile;

    if (index == 0) {
      String path = String(filePrefix) + "/" + filename;
      updateFile = SPIFFS.open(path, "w");

      if (!updateFile) {
        Serial.println(F("Failed to open file"));
        request->send(500);
        return;
      }
    }

    if (!updateFile || updateFile.write(data, len) != len) {
      Serial.println(F("Failed to write to file"));
      request->send(500);
    }

    if (updateFile && isFinal) {
      updateFile.close();
      request->send(200);
    }
  };
}

void WebServer::handleUpdateJsonFile(const String& path, AsyncWebServerRequest* request, uint8_t* data, size_t len) {
  DynamicJsonBuffer requestBuffer;
  JsonObject& body = requestBuffer.parseObject(data);

  if (! body.success()) {
    request->send_P(400, TEXT_PLAIN, PSTR("Invalid JSON"));
    return;
  }

  if (SPIFFS.exists(path)) {
    File file = SPIFFS.open(path, "r");

    DynamicJsonBuffer fileBuffer;
    JsonObject& tmpl = fileBuffer.parse(file);
    file.close();

    if (! tmpl.success()) {
      request->send_P(500, TEXT_PLAIN, PSTR("Failed to load persisted file"));
      return;
    }

    for (JsonObject::iterator itr = body.begin(); itr != body.end(); ++itr) {
      tmpl[itr->key] = itr->value;
    }

    file = SPIFFS.open(path, "w");
    tmpl.printTo(file);
    file.close();

    String response;
    tmpl.printTo(response);
    request->send(200, APPLICATION_JSON, response);
  } else {
    request->send(404, TEXT_PLAIN);
  }
}

ArBodyHandlerFunction WebServer::handleUpdateSettings() {
  return [this](
    AsyncWebServerRequest* request,
    uint8_t* data,
    size_t len,
    size_t index,
    size_t total
  ) {
    DynamicJsonBuffer buffer;
    JsonObject& req = buffer.parse(data);

    if (! req.success()) {
      request->send_P(400, TEXT_PLAIN, PSTR("Invalid JSON"));
      return;
    }

    settings.patch(req);
    settings.save();

    request->send(200);
  };
}

ArRequestHandlerFunction WebServer::handleListSettings() {
  return [this](AsyncWebServerRequest* request) {
    request->send(200, APPLICATION_JSON, settings.toJson());
  };
}

ArUploadHandlerFunction WebServer::handleUpdateFile(const char* filename) {
  return [this, filename](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool isFinal) {
    if (index == 0) {
      updateFile = SPIFFS.open(filename, "w");
    } else if (! isFinal) {
      if (updateFile.write(data, len) != len) {
        Serial.println(F("Error updating web file"));
      }
    } else {
      updateFile.close();
      request->send_P(200, TEXT_PLAIN, PSTR("success"));
    }
  };
}

bool WebServer::isAuthenticated(AsyncWebServerRequest* request) {
  if (settings.hasAuthSettings()) {
    if (request->authenticate(settings.adminUsername.c_str(), settings.adminPassword.c_str())) {
      return true;
    } else {
      request->send_P(403, TEXT_PLAIN, PSTR("Authentication required"));
      return false;
    }
  } else {
    return true;
  }
}

void WebServer::onPattern(const String& pattern, const WebRequestMethod method, PatternHandler::TPatternHandlerFn fn) {
  PatternHandler::TPatternHandlerFn authedFn = [this, fn](const UrlTokenBindings* b, AsyncWebServerRequest* request) {
    if (isAuthenticated(request)) {
      fn(b, request);
    }
  };

  server.addHandler(new PatternHandler(pattern.c_str(), method, authedFn, NULL));
}

void WebServer::onPattern(const String& pattern, const WebRequestMethod method, PatternHandler::TPatternHandlerBodyFn fn) {
  PatternHandler::TPatternHandlerBodyFn authedFn = [this, fn](
    const UrlTokenBindings* bindings,
    AsyncWebServerRequest* request,
    uint8_t* data,
    size_t len,
    size_t index,
    size_t total
  ) {
    if (isAuthenticated(request)) {
      fn(bindings, request, data, len, index, total);
    }
  };

  server.addHandler(new PatternHandler(pattern.c_str(), method, NULL, authedFn));
}

void WebServer::on(const String& path, const WebRequestMethod method, ArRequestHandlerFunction fn) {
  ArRequestHandlerFunction authedFn = [this, fn](AsyncWebServerRequest* request) {
    if (isAuthenticated(request)) {
      fn(request);
    }
  };

  server.on(path.c_str(), method, authedFn);
}

void WebServer::on(const String& path, const WebRequestMethod method, ArBodyHandlerFunction fn) {
  ArBodyHandlerFunction authedFn = [this, fn](
    AsyncWebServerRequest* request,
    uint8_t* data,
    size_t len,
    size_t index,
    size_t total
  ) {
    if (isAuthenticated(request)) {
      fn(request, data, len, index, total);
    }
  };

  server.addHandler(new WebServer::BodyHandler(path.c_str(), method, authedFn));
}

void WebServer::onUpload(const String& path, const WebRequestMethod method, ArUploadHandlerFunction fn) {
  ArUploadHandlerFunction authedFn = [this, fn](
    AsyncWebServerRequest *request,
    const String& filename,
    size_t index,
    uint8_t *data,
    size_t len,
    bool isFinal
  ) {
    if (isAuthenticated(request)) {
      fn(request, filename, index, data, len, isFinal);
    }
  };

  server.addHandler(new WebServer::UploadHandler(path.c_str(), method, authedFn));
}

WebServer::UploadHandler::UploadHandler(
  const char* uri,
  const WebRequestMethod method,
  ArUploadHandlerFunction handler
) : uri(new char[strlen(uri) + 1]),
    method(method),
    handler(handler)
{
  strcpy(this->uri, uri);
}

WebServer::UploadHandler::~UploadHandler() {
  delete uri;
}

bool WebServer::UploadHandler::canHandle(AsyncWebServerRequest *request) {
  if (this->method != HTTP_ANY && this->method != request->method()) {
    return false;
  }

  return request->url() == this->uri;
}

void WebServer::UploadHandler::handleUpload(
  AsyncWebServerRequest *request,
  const String &filename,
  size_t index,
  uint8_t *data,
  size_t len,
  bool isFinal
) {
  handler(request, filename, index, data, len, isFinal);
}

WebServer::BodyHandler::BodyHandler(
  const char* uri,
  const WebRequestMethod method,
  ArBodyHandlerFunction handler
) : uri(new char[strlen(uri) + 1]),
    method(method),
    handler(handler)
{
  strcpy(this->uri, uri);
}

WebServer::BodyHandler::~BodyHandler() {
  delete uri;
}

bool WebServer::BodyHandler::canHandle(AsyncWebServerRequest *request) {
  if (this->method != HTTP_ANY && this->method != request->method()) {
    return false;
  }

  return request->url() == this->uri;
}

void WebServer::BodyHandler::handleBody(
  AsyncWebServerRequest *request,
  uint8_t *data,
  size_t len,
  size_t index,
  size_t total
) {
  handler(request, data, len, index, total);
}