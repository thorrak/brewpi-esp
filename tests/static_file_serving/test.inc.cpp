int main(int argc, char** argv) {
    assert(argc == 2);
    root = argv[1];
    std::filesystem::create_directory(root + "/assets");
    std::ofstream(root + "/index.html") << "web UI";
    std::ofstream(root + "/assets/app.js.gz") << "compressed asset";
    for (const char* name : {"manifest.json", "records.bin", "finish.json", "ack.json",
                            "boots.json", "resumed.json", "reserve.bin", "manifest.json.tmp"}) {
        std::string file = std::string("water-test-") + name;
        std::ofstream(root + "/" + file) << "private data";
        for (const char* prefix : {"/", "//", "/./", "///./", "/assets/../", "/%2e/"}) {
            std::string uri = std::string(prefix) + file;
            httpd_req_t request{uri.c_str(), {}, {}, {}};
            filesystemCalls = 0;
            assert(httpServer::handleFileRead(&request, request.uri) == ESP_FAIL);
            assert(request.body.empty());
            assert(filesystemCalls == 0);
            assert(httpServer::not_found_handler(&request, 404) == ESP_OK);
            assert(request.body == "Not Found");
            assert(filesystemCalls == 0);
        }
    }
    httpd_req_t index{"/", {}, {}, {}};
    assert(httpServer::handleFileRead(&index, index.uri) == ESP_OK);
    assert(index.body == "web UI" && index.type == "text/html");
    httpd_req_t page{"/water-test", {}, {}, {}};
    assert(httpServer::static_file_handler(&page) == ESP_OK);
    assert(page.body == "web UI");
    httpd_req_t asset{"/assets/app.js", {}, {}, {}};
    assert(httpServer::not_found_handler(&asset, 404) == ESP_OK);
    assert(asset.body == "compressed asset");
    assert(asset.type == "application/javascript");
    assert(asset.headers["Content-Encoding"] == "gzip");
    httpd_req_t missing{"/missing.js", {}, {}, {}};
    assert(httpServer::not_found_handler(&missing, 404) == ESP_OK);
    assert(missing.body == "Not Found");
    // Never truncate a request to another existing filename.
    std::string longPath = "/" + std::string(255, 'a');
    httpd_req_t oversized{longPath.c_str(), {}, {}, {}};
    filesystemCalls = 0;
    assert(httpServer::handleFileRead(&oversized, oversized.uri) == ESP_FAIL);
    assert(filesystemCalls == 0);
    std::puts("static_file_serving: private aliases blocked; normal assets and SPA routes served");
}
