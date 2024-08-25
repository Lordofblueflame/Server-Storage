#include <data/searching/search_result.h>
#include "pch.h"

Search_Result::Search_Result( std::string output) : output(output) {}


void Search_Result::display()  {
    std::cout << "Search Results:\n" << output << std::endl;
}