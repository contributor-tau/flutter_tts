#include "include/flutter_tts/flutter_tts_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <cstring>
#include <memory>
#include <string>

#include "tts_engine.h"

namespace {

constexpr char kChannelName[] = "flutter_tts";

// Forwards engine events to Dart over the method channel.
class ChannelListener : public flutter_tts::TtsEngine::Listener {
 public:
  explicit ChannelListener(FlMethodChannel* channel) : channel_(channel) {}

  void OnSpeakStart() override { Invoke("speak.onStart", nullptr); }
  void OnSpeakComplete() override { Invoke("speak.onComplete", nullptr); }
  void OnSpeakCancel() override { Invoke("speak.onCancel", nullptr); }
  void OnSpeakPause() override { Invoke("speak.onPause", nullptr); }
  void OnSpeakContinue() override { Invoke("speak.onContinue", nullptr); }

  void OnSpeakProgress(const std::string& text,
                       size_t start,
                       size_t end,
                       const std::string& word) override {
    FlValue* args = fl_value_new_map();
    fl_value_set_string_take(args, "text", fl_value_new_string(text.c_str()));
    fl_value_set_string_take(args, "start",
                             fl_value_new_int(static_cast<int64_t>(start)));
    fl_value_set_string_take(args, "end",
                             fl_value_new_int(static_cast<int64_t>(end)));
    fl_value_set_string_take(args, "word", fl_value_new_string(word.c_str()));
    Invoke("speak.onProgress", args);
  }

  void OnSpeakError(const std::string& message) override {
    Invoke("speak.onError", fl_value_new_string(message.c_str()));
  }

 private:
  // Takes ownership of |args|.
  void Invoke(const char* method, FlValue* args) {
    g_autoptr(FlValue) owned_args = args;
    fl_method_channel_invoke_method(channel_, method, owned_args, nullptr,
                                    nullptr, nullptr);
  }

  FlMethodChannel* channel_;  // Not owned.
};

// Keeps an FlMethodCall alive until the result is delivered.
using MethodCallRef = std::shared_ptr<FlMethodCall>;

MethodCallRef RefMethodCall(FlMethodCall* call) {
  return MethodCallRef(FL_METHOD_CALL(g_object_ref(call)),
                       [](FlMethodCall* c) { g_object_unref(c); });
}

void Respond(FlMethodCall* call, FlValue* result) {
  g_autoptr(FlValue) owned = result;
  g_autoptr(GError) error = nullptr;
  if (!fl_method_call_respond_success(call, owned, &error)) {
    g_warning("flutter_tts: failed to send response: %s", error->message);
  }
}

void RespondInt(FlMethodCall* call, int value) {
  Respond(call, fl_value_new_int(value));
}

FlValue* StringList(const std::vector<std::string>& items) {
  FlValue* list = fl_value_new_list();
  for (const std::string& item : items) {
    fl_value_append_take(list, fl_value_new_string(item.c_str()));
  }
  return list;
}

bool GetString(FlValue* value, std::string* out) {
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_STRING) {
    return false;
  }
  *out = fl_value_get_string(value);
  return true;
}

bool GetDouble(FlValue* value, double* out) {
  if (value == nullptr)
    return false;
  switch (fl_value_get_type(value)) {
    case FL_VALUE_TYPE_FLOAT:
      *out = fl_value_get_float(value);
      return true;
    case FL_VALUE_TYPE_INT:
      *out = static_cast<double>(fl_value_get_int(value));
      return true;
    default:
      return false;
  }
}

bool GetBool(FlValue* value, bool* out) {
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_BOOL) {
    return false;
  }
  *out = fl_value_get_bool(value);
  return true;
}

bool GetInt(FlValue* value, int64_t* out) {
  if (value == nullptr)
    return false;
  if (fl_value_get_type(value) == FL_VALUE_TYPE_INT) {
    *out = fl_value_get_int(value);
    return true;
  }
  std::string s;
  if (GetString(value, &s)) {
    char* end = nullptr;
    const long long parsed = g_ascii_strtoll(s.c_str(), &end, 10);
    if (end != s.c_str() && *end == '\0') {
      *out = parsed;
      return true;
    }
  }
  return false;
}

// Looks up a string entry of a map argument; missing entries yield "".
std::string MapString(FlValue* map, const char* key) {
  if (map == nullptr || fl_value_get_type(map) != FL_VALUE_TYPE_MAP)
    return "";
  std::string result;
  GetString(fl_value_lookup_string(map, key), &result);
  return result;
}

}  // namespace

#define FLUTTER_TTS_PLUGIN(obj)                                     \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), flutter_tts_plugin_get_type(), \
                              FlutterTtsPlugin))

struct _FlutterTtsPlugin {
  GObject parent_instance;
  FlMethodChannel* channel;
  ChannelListener* listener;
  flutter_tts::TtsEngine* engine;
};

G_DEFINE_TYPE(FlutterTtsPlugin, flutter_tts_plugin, g_object_get_type())

static void flutter_tts_plugin_handle_method_call(FlutterTtsPlugin* self,
                                                  FlMethodCall* method_call) {
  flutter_tts::TtsEngine* engine = self->engine;
  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);

  if (strcmp(method, "speak") == 0) {
    std::string text;
    // Other platforms receive a plain string; accept Android's map form too.
    if (!GetString(args, &text))
      text = MapString(args, "text");
    MethodCallRef call = RefMethodCall(method_call);
    engine->Speak(text, [call](int result) { RespondInt(call.get(), result); });
  } else if (strcmp(method, "pause") == 0) {
    engine->Pause();
    RespondInt(method_call, 1);
  } else if (strcmp(method, "stop") == 0) {
    engine->Stop();
    RespondInt(method_call, 1);
  } else if (strcmp(method, "awaitSpeakCompletion") == 0) {
    bool await = false;
    if (GetBool(args, &await))
      engine->SetAwaitSpeakCompletion(await);
    RespondInt(method_call, 1);
  } else if (strcmp(method, "setQueueMode") == 0) {
    int64_t mode = 0;
    if (GetInt(args, &mode))
      engine->SetQueueMode(static_cast<int>(mode));
    RespondInt(method_call, 1);
  } else if (strcmp(method, "setSpeechRate") == 0) {
    double value = 0;
    RespondInt(method_call,
               GetDouble(args, &value) && engine->SetSpeechRate(value) ? 1 : 0);
  } else if (strcmp(method, "setVolume") == 0) {
    double value = 0;
    RespondInt(method_call,
               GetDouble(args, &value) && engine->SetVolume(value) ? 1 : 0);
  } else if (strcmp(method, "setPitch") == 0) {
    double value = 0;
    RespondInt(method_call,
               GetDouble(args, &value) && engine->SetPitch(value) ? 1 : 0);
  } else if (strcmp(method, "getSpeechRateValidRange") == 0) {
    FlValue* range = fl_value_new_map();
    fl_value_set_string_take(range, "min", fl_value_new_float(0.0));
    fl_value_set_string_take(range, "normal", fl_value_new_float(0.5));
    fl_value_set_string_take(range, "max", fl_value_new_float(1.0));
    fl_value_set_string_take(range, "platform", fl_value_new_string("linux"));
    Respond(method_call, range);
  } else if (strcmp(method, "getLanguages") == 0) {
    Respond(method_call, StringList(engine->GetLanguages()));
  } else if (strcmp(method, "isLanguageAvailable") == 0) {
    std::string language;
    Respond(method_call,
            fl_value_new_bool(GetString(args, &language) &&
                              engine->IsLanguageAvailable(language)));
  } else if (strcmp(method, "setLanguage") == 0) {
    std::string language;
    RespondInt(
        method_call,
        GetString(args, &language) && engine->SetLanguage(language) ? 1 : 0);
  } else if (strcmp(method, "getVoices") == 0) {
    FlValue* list = fl_value_new_list();
    for (const auto& voice : engine->GetVoices()) {
      FlValue* map = fl_value_new_map();
      fl_value_set_string_take(map, "name",
                               fl_value_new_string(voice.name.c_str()));
      fl_value_set_string_take(map, "locale",
                               fl_value_new_string(voice.locale.c_str()));
      fl_value_append_take(list, map);
    }
    Respond(method_call, list);
  } else if (strcmp(method, "setVoice") == 0) {
    const std::string name = MapString(args, "name");
    RespondInt(
        method_call,
        !name.empty() && engine->SetVoice(name, MapString(args, "locale")) ? 1
                                                                           : 0);
  } else if (strcmp(method, "clearVoice") == 0) {
    RespondInt(method_call, engine->ClearVoice() ? 1 : 0);
  } else if (strcmp(method, "getEngines") == 0) {
    Respond(method_call, StringList(engine->GetEngines()));
  } else if (strcmp(method, "getDefaultEngine") == 0) {
    const std::string name = engine->GetDefaultEngine();
    Respond(method_call, name.empty() ? fl_value_new_null()
                                      : fl_value_new_string(name.c_str()));
  } else if (strcmp(method, "setEngine") == 0) {
    std::string name;
    RespondInt(method_call,
               GetString(args, &name) && engine->SetEngine(name) ? 1 : 0);
  } else {
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond_not_implemented(method_call, &error);
  }
}

static void method_call_cb(FlMethodChannel* channel,
                           FlMethodCall* method_call,
                           gpointer user_data) {
  flutter_tts_plugin_handle_method_call(FLUTTER_TTS_PLUGIN(user_data),
                                        method_call);
}

static void flutter_tts_plugin_dispose(GObject* object) {
  FlutterTtsPlugin* self = FLUTTER_TTS_PLUGIN(object);
  // The engine must go first: it refers to the listener.
  delete self->engine;
  self->engine = nullptr;
  delete self->listener;
  self->listener = nullptr;
  g_clear_object(&self->channel);
  G_OBJECT_CLASS(flutter_tts_plugin_parent_class)->dispose(object);
}

static void flutter_tts_plugin_class_init(FlutterTtsPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = flutter_tts_plugin_dispose;
}

static void flutter_tts_plugin_init(FlutterTtsPlugin* self) {
  self->channel = nullptr;
  self->listener = nullptr;
  self->engine = nullptr;
}

void flutter_tts_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  FlutterTtsPlugin* plugin =
      FLUTTER_TTS_PLUGIN(g_object_new(flutter_tts_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  plugin->channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            kChannelName, FL_METHOD_CODEC(codec));
  // Speech Dispatcher is connected to lazily, on first use, so apps that
  // never speak don't pay for it at startup.
  plugin->listener = new ChannelListener(plugin->channel);
  plugin->engine = new flutter_tts::TtsEngine(plugin->listener);

  fl_method_channel_set_method_call_handler(
      plugin->channel, method_call_cb, g_object_ref(plugin), g_object_unref);

  g_object_unref(plugin);
}
