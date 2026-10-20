// ============================================================================
// MultiSessionChatter - C++ 单文件版 (GTK3 弹窗)
// 编译:
//   g++ -std=c++17 -O2 main.cpp -o aichat -lcurl $(pkg-config --cflags --libs gtk+-3.0)
// 依赖: libcurl + nlohmann/json + gtk+-3.0
// ============================================================================

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <gtk/gtk.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

static constexpr const char* CONFIG_FILE = "config.ini";

// ====================== 工具 ======================
static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// ====================== HTTP 层 ======================
static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

struct HttpResponse {
    long code = 0;
    std::string body;
    std::string error;
};

static HttpResponse http_request(const std::string& url,
                                 const std::string& api_key,
                                 const std::string& method,
                                 const std::string& body = "") {
    HttpResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) { resp.code = -1; resp.error = "curl_easy_init failed"; return resp; }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    std::string auth = "Authorization: Bearer " + api_key;
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        resp.code = -1;
        resp.error = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

// ====================== GTK 弹窗 ======================
namespace {

struct InputState {
    GtkWidget* textview = nullptr;
    std::string result;
    bool ok = false;
};

static void on_window_destroy_quit(GtkWidget*, gpointer) { gtk_main_quit(); }

static void on_input_ok(GtkButton*, gpointer data) {
    auto* st = static_cast<InputState*>(data);
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->textview));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar* text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    st->result = text ? text : "";
    g_free(text);
    st->ok = true;
    gtk_main_quit();
}

static void on_input_cancel(GtkButton*, gpointer data) {
    auto* st = static_cast<InputState*>(data);
    st->ok = false;
    gtk_main_quit();
}

static void on_info_close(GtkButton*, gpointer) { gtk_main_quit(); }

}  // namespace

// 多行输入弹窗；点 Cancel 或关闭窗口返回空字符串
static std::string inputBox(const std::string& title, const std::string& content) {
    InputState st;

    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), title.c_str());
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 420);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
    g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy_quit), nullptr);
    g_signal_connect(win, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) {
        gtk_main_quit();
        return FALSE;
    }), nullptr);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget* label = gtk_label_new(content.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    GtkWidget* tv = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scrolled), tv);
    st.textview = tv;

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    GtkWidget* cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_input_cancel), &st);
    gtk_box_pack_end(GTK_BOX(hbox), cancel_btn, FALSE, FALSE, 0);

    GtkWidget* ok_btn = gtk_button_new_with_label("OK");
    gtk_widget_set_can_default(ok_btn, TRUE);
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_input_ok), &st);
    gtk_box_pack_end(GTK_BOX(hbox), ok_btn, FALSE, FALSE, 0);
    gtk_widget_grab_default(ok_btn);

    gtk_widget_show_all(win);
    gtk_widget_grab_focus(tv);
    gtk_main();
    gtk_widget_destroy(win);

    return st.ok ? st.result : std::string();
}

// 只读回复弹窗
static void infoBox(const std::string& content) {
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Reply");
    gtk_window_set_default_size(GTK_WINDOW(win), 640, 520);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
    g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy_quit), nullptr);
    g_signal_connect(win, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) {
        gtk_main_quit();
        return FALSE;
    }), nullptr);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    GtkWidget* tv = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(buf, content.c_str(), -1);
    gtk_container_add(GTK_CONTAINER(scrolled), tv);

    GtkWidget* close_btn = gtk_button_new_with_label("Close");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_info_close), nullptr);
    gtk_box_pack_end(GTK_BOX(vbox), close_btn, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    gtk_main();
    gtk_widget_destroy(win);
}

// ====================== 配置 ======================
struct Config {
    std::string base_url;
    std::string api_key;
    std::string default_model;
};

static Config load_config(const std::string& path) {
    Config cfg;
    if (!fs::exists(path)) {
        std::ofstream ofs(path);
        ofs << "[api]\nbase_url=\napi_key=\n\n[model]\ndefault=claude-sonnet-5\n";
        std::cerr << "[Config] '" << path
                  << "' not found, template created. Fill it and rerun.\n";
        std::exit(0);
    }
    std::ifstream ifs(path);
    if (!ifs) { std::cerr << "[Config] Cannot open " << path << "\n"; std::exit(1); }

    std::string line, section;
    while (std::getline(ifs, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            auto e = line.find(']');
            if (e != std::string::npos) section = line.substr(1, e - 1);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (section == "api") {
            if (key == "base_url") cfg.base_url = val;
            else if (key == "api_key") cfg.api_key = val;
        } else if (section == "model") {
            if (key == "default") cfg.default_model = val;
        }
    }

    if (cfg.base_url.empty() || cfg.api_key.empty() || cfg.api_key == "sk-REPLACE_ME") {
        std::cerr << "[Config] Please fill valid base_url / api_key in " << path << "\n";
        std::exit(1);
    }
    return cfg;
}

// ====================== 主体 ======================
class MultiSessionChatter {
public:
    MultiSessionChatter(const std::string& base_url,
                        const std::string& api_key,
                        const std::string& default_model = "",
                        const std::string& cache_dir = "./.bot-history",
                        int max_completion_tokens = 1024,
                        double temperature = 1.0,
                        double top_p = 0.95)
        : base_url_(base_url),
          api_key_(api_key),
          cache_dir_(cache_dir),
          max_tokens_(max_completion_tokens),
          temperature_(temperature),
          top_p_(top_p) {
        fs::create_directories(cache_dir_);
        system_prompt_ = json{{"role", "system"}, {"content", "You are an AI assistant."}};
        fetch_models();

        if (!default_model.empty() &&
            std::find(available_models_.begin(), available_models_.end(), default_model) !=
                available_models_.end()) {
            model_ = default_model;
        } else if (!default_model.empty()) {
            std::cout << "[Models] Warning: '" << default_model
                      << "' not in server model list, still using it.\n";
            model_ = default_model;
        } else if (!available_models_.empty()) {
            model_ = available_models_.front();
        } else {
            model_ = "gpt-4o";
        }
    }

    std::vector<std::string> list_models(const std::string& keyword = "") const {
        if (keyword.empty()) return available_models_;
        std::string kw = to_lower(keyword);
        std::vector<std::string> out;
        for (const auto& m : available_models_) {
            if (to_lower(m).find(kw) != std::string::npos) out.push_back(m);
        }
        return out;
    }

    std::string current_model() const { return model_; }

    std::string switch_model(const std::string& name) {
        if (name.empty()) {
            return "Current model: " + model_ + "\nAvailable models: " +
                   std::to_string(available_models_.size()) +
                   " (use 'model list [keyword]' to list them)";
        }
        if (std::find(available_models_.begin(), available_models_.end(), name) !=
            available_models_.end()) {
            model_ = name;
            return "Model switched to '" + name + "'.";
        }
        std::string nl = to_lower(name);
        for (const auto& m : available_models_) {
            if (to_lower(m) == nl) {
                model_ = m;
                return "Model switched to '" + m + "'.";
            }
        }
        model_ = name;
        return "Warning: '" + name +
               "' not in server model list, but set as current model anyway.";
    }

    std::string create_session(const std::string& id, const std::string& inherit_from = "") {
        if (sessions_.count(id)) return "Session " + id + " Exist!";
        json msgs = json::array();
        msgs.push_back(system_prompt_);
        if (!inherit_from.empty() && sessions_.count(inherit_from)) {
            const json& src = sessions_[inherit_from];
            if (src.is_array()) {
                for (size_t i = 1; i < src.size(); ++i) msgs.push_back(src[i]);
            }
        }
        sessions_[id] = msgs;
        save_session(id);
        return "Session " + id + " Created.";
    }

    std::vector<std::string> list_sessions() const {
        std::vector<std::string> out;
        for (const auto& kv : sessions_) out.push_back(kv.first);
        return out;
    }

    std::string clear_session(const std::string& id) {
        if (!sessions_.count(id)) return "Session " + id + " Doesn't Exist!";
        json fresh = json::array();
        fresh.push_back(system_prompt_);
        sessions_[id] = fresh;
        save_session(id);
        return "Session " + id + " Cleaned.";
    }

    std::string delete_session(const std::string& id) {
        sessions_.erase(id);
        auto p = session_path(id);
        std::error_code ec;
        if (fs::exists(p, ec)) fs::remove(p, ec);
        return "Session " + id + " Deleted.";
    }

    std::string load_all_sessions() {
        if (!fs::exists(cache_dir_)) return "Include 0 Session(s).";
        for (const auto& entry : fs::directory_iterator(cache_dir_)) {
            if (!entry.is_regular_file()) continue;
            auto p = entry.path();
            if (p.extension() != ".json") continue;
            try {
                std::ifstream ifs(p);
                json j;
                ifs >> j;
                if (!j.is_array()) continue;
                sessions_[p.stem().string()] = j;
            } catch (...) {
                continue;
            }
        }
        return "Include " + std::to_string(sessions_.size()) + " Session(s).";
    }

    std::string chat(const std::string& id, const std::string& user_input) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return "Please create session " + id + " first.";
        json& msgs = it->second;
        if (!msgs.is_array()) msgs = json::array();

        msgs.push_back(json{{"role", "user"}, {"content", user_input}});

        json req = {
            {"model", model_},
            {"messages", msgs},
            {"max_completion_tokens", max_tokens_},
            {"temperature", temperature_},
            {"top_p", top_p_},
        };

        auto resp = http_request(join_url("chat/completions"), api_key_, "POST", req.dump());
        if (resp.code != 200) {
            msgs.erase(msgs.size() - 1);
            std::string err = (resp.code < 0) ? resp.error : resp.body;
            return "Error: HTTP " + std::to_string(resp.code) + " " + err;
        }

        try {
            auto j = json::parse(resp.body);
            std::string content = j["choices"][0]["message"]["content"].get<std::string>();
            content = trim(content);
            msgs.push_back(json{{"role", "assistant"}, {"content", content}});
            save_session(id);
            return content;
        } catch (const std::exception& e) {
            msgs.erase(msgs.size() - 1);
            return std::string("Error: ") + e.what();
        }
    }

    std::string dump_models_to_file(const std::string& path = "models.txt") {
        std::ofstream ofs(path);
        for (const auto& m : available_models_) ofs << m << "\n";
        return "Wrote " + std::to_string(available_models_.size()) + " models to " + path + ".";
    }

private:
    std::string base_url_;
    std::string api_key_;
    std::string cache_dir_;
    int max_tokens_;
    double temperature_;
    double top_p_;

    std::vector<std::string> available_models_;
    std::string model_;
    std::map<std::string, json> sessions_;
    json system_prompt_;

    std::string join_url(const std::string& path) const {
        std::string u = base_url_;
        while (!u.empty() && u.back() == '/') u.pop_back();
        return u + "/" + path;
    }

    std::string session_path(const std::string& id) const {
        return (fs::path(cache_dir_) / (id + ".json")).string();
    }

    void save_session(const std::string& id) {
        std::ofstream ofs(session_path(id));
        ofs << sessions_[id].dump(2) << "\n";
    }

    void fetch_models() {
        auto resp = http_request(join_url("models"), api_key_, "GET");
        if (resp.code == 200) {
            try {
                json j = json::parse(resp.body);
                std::set<std::string> ids;
                if (j.contains("data") && j["data"].is_array()) {
                    for (const auto& m : j["data"]) {
                        if (m.is_object() && m.contains("id") && m["id"].is_string())
                            ids.insert(m["id"].get<std::string>());
                    }
                }
                available_models_.assign(ids.begin(), ids.end());
                std::cout << "[Models] Fetched " << available_models_.size()
                          << " models from server.\n";
            } catch (const std::exception& e) {
                std::cout << "[Models] Parse error: " << e.what() << "\n";
            }
        } else {
            std::cout << "[Models] Failed to fetch model list (HTTP " << resp.code << ")";
            if (resp.code < 0 && !resp.error.empty()) std::cout << ": " << resp.error;
            else if (!resp.body.empty()) std::cout << ": " << resp.body;
            std::cout << "\n";
        }
    }
};

// ====================== 入口 ======================
int main(int argc, char** argv) {
    try {
        // GTK 只初始化一次；之后每次弹窗各自跑一段 gtk_main 循环
        gtk_init(&argc, &argv);

        Config cfg = load_config(CONFIG_FILE);

        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            std::cerr << "curl_global_init failed\n";
            return 1;
        }

        MultiSessionChatter bot(cfg.base_url, cfg.api_key, cfg.default_model);
        std::cout << bot.load_all_sessions() << "\n";
        std::cout << "Current model: " << bot.current_model() << "\n";

        std::cout <<
            "\nUsage: \n"
            "  create [SessionID]                 新建会话\n"
            "  create [NewID] [FromID]            继承历史新建会话\n"
            "  chat [SessionID] [Message]         发消息（不带消息则弹窗输入）\n"
            "  list                               列出所有会话\n"
            "  clear [SessionID]                  清空会话（保留 system）\n"
            "  delete [SessionID]                 删除会话\n"
            "  model                              查看当前模型\n"
            "  model list [keyword]               列出可用模型（可带关键词过滤）\n"
            "  model <Name>                       切换模型（全局生效）\n"
            "  models-dump                        把模型清单写入 models.txt\n"
            "  quit / exit                        退出\n";

        std::string cmd;
        while (true) {
            std::cout << "\n> " << std::flush;
            if (!std::getline(std::cin, cmd)) break;
            if (!cmd.empty() && cmd.back() == '\r') cmd.pop_back();
            cmd = trim(cmd);
            if (cmd.empty()) continue;

            std::string lc = to_lower(cmd);

            if (lc == "quit" || lc == "exit") {
                std::cout << "Exited.\n";
                break;
            }

            if (cmd == "list") {
                auto sessions = bot.list_sessions();
                std::cout << "Session List: [";
                for (size_t i = 0; i < sessions.size(); ++i) {
                    if (i) std::cout << ", ";
                    std::cout << sessions[i];
                }
                std::cout << "]\n";
                continue;
            }

            if (cmd == "model") {
                std::cout << "Current model: " << bot.current_model() << "\n";
                std::cout << "Available models: " << bot.list_models().size()
                          << " (use 'model list [keyword]' to view)\n";
                continue;
            }

            if (cmd == "model list") {
                int i = 1;
                for (const auto& m : bot.list_models()) {
                    std::cout << "  " << i++ << ". " << m
                              << (m == bot.current_model() ? " *" : "") << "\n";
                }
                continue;
            }

            if (cmd.rfind("model list ", 0) == 0) {
                std::string kw = trim(cmd.substr(11));
                auto results = bot.list_models(kw);
                if (results.empty()) {
                    std::cout << "No model matches '" << kw << "'.\n";
                } else {
                    for (const auto& m : results)
                        std::cout << "  " << m
                                  << (m == bot.current_model() ? " *" : "") << "\n";
                }
                continue;
            }

            if (cmd.rfind("model ", 0) == 0) {
                std::cout << bot.switch_model(trim(cmd.substr(6))) << "\n";
                continue;
            }

            if (cmd == "models-dump") {
                std::cout << bot.dump_models_to_file() << "\n";
                continue;
            }

            if (cmd.rfind("create ", 0) == 0) {
                std::istringstream iss(cmd.substr(7));
                std::string id, from;
                iss >> id >> from;
                if (id.empty()) std::cout << "Usage: create [SessionID] [InheritFrom?]\n";
                else std::cout << bot.create_session(id, from) << "\n";
                continue;
            }

            if (cmd.rfind("clear ", 0) == 0) {
                std::cout << bot.clear_session(trim(cmd.substr(6))) << "\n";
                continue;
            }

            if (cmd.rfind("delete ", 0) == 0) {
                std::cout << bot.delete_session(trim(cmd.substr(7))) << "\n";
                continue;
            }

            if (cmd.rfind("chat ", 0) == 0) {
                std::string rest = cmd.substr(5);
                size_t sp = rest.find(' ');
                std::string session = (sp == std::string::npos) ? rest : rest.substr(0, sp);
                std::string message = (sp == std::string::npos) ? "" : trim(rest.substr(sp + 1));
                if (session.empty()) {
                    std::cout << "Usage: chat [SessionID] {Message}\n";
                    continue;
                }
                if (!message.empty()) {
                    std::cout << bot.chat(session, message) << "\n";
                } else {
                    // 弹窗输入 → 弹窗显示回复（与 Python tkinter 版一致）
                    std::string text = inputBox("AI Chatter", "What do you want to say?");
                    if (!text.empty()) {
                        infoBox(bot.chat(session, text));
                    }
                }
                continue;
            }

            std::cout << "Unknown Command.\n";
        }

        curl_global_cleanup();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}