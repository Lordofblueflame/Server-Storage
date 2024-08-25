#pragma once
#include <string>

class Search_Result {
public:
    Search_Result(std::string output);

public:
    void display();

private:
    std::string output; 
};
