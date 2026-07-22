#pragma once
#include <vector>
#include "ast.hpp"
#include "token.hpp"

Program parse(std::vector<Token> toks);
