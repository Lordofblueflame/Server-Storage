#ifndef PCH_H
#define PCH_H

//Precompiled header
//other
#include <boost/log/trivial.hpp>
#include <iostream>
#include <boost/config.hpp>
#include <iomanip> 

//web
#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>

//memory
#include <boost/thread.hpp>
#include <thread>
#include <memory>

//files
#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>
#include <boost/property_tree/ptree.hpp> 
#include <boost/property_tree/json_parser.hpp>

//data structures
#include <string>
#include <map>
#include <vector>

//namepsaces
namespace beast = boost::beast;         
namespace http = beast::http;           
namespace net = boost::asio;
namespace fs = boost::filesystem; 
namespace pt = boost::property_tree;       
using tcp = boost::asio::ip::tcp;  

//static config
static const std::string getProjectDir() {
    //TO DO
    // I need better solution propably with config file and config class 
    // Prepare list of things that need configuration
    // Create config.json
    // Cache filelist.json
    fs::path currentPath = fs::current_path();
    fs::path projectDir = currentPath.parent_path().parent_path();
    
    projectDir = fs::absolute(projectDir);

    BOOST_LOG_TRIVIAL(info) << "Project Directory: " << projectDir.string();
    return projectDir.string();
}

#endif //PCH_H