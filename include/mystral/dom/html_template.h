#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mystral::dom {

enum class NodeType {
  Element = 1,
  Text = 3,
  Comment = 8,
  DocumentFragment = 11,
};

struct ParsedNode {
  NodeType type = NodeType::DocumentFragment;
  std::string name;
  std::string text;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<std::unique_ptr<ParsedNode>> children;
};

std::unique_ptr<ParsedNode> parseHTMLTemplate(const std::string &html,
                                              std::string &error);

} // namespace mystral::dom
