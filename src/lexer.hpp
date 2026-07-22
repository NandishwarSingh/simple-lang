#pragma once
#include <string>
#include <vector>
#include "token.hpp"

std::vector<Token> lex(const std::string& src);
