#include "JsonKeys.h"
#include "GlycolParams.h"

GlycolConfig::GlycolConfig() {
    setDefaults();
}

void GlycolConfig::setDefaults() {
    trigger_margin = 0.1f;
}

void GlycolConfig::toJson(JsonDocument& doc) {
    doc[GlycolKeys::trigger_margin] = trigger_margin;
}

void GlycolConfig::storeToFilesystem() {
    JsonDocument doc;
    toJson(doc);
    writeJsonToFile(GlycolConfig::filename, doc);
}

void GlycolConfig::loadFromFilesystem() {
    setDefaults();
    JsonDocument json_doc = readJsonFromFile(GlycolConfig::filename);

    if (json_doc[GlycolKeys::trigger_margin].is<float>()) trigger_margin = json_doc[GlycolKeys::trigger_margin];
}
