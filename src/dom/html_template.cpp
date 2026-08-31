#include "mystral/dom/html_template.h"

#ifdef MYSTRAL_HAS_LEXBOR
#include <lexbor/html/html.h>
#endif

namespace mystral::dom {

#ifdef MYSTRAL_HAS_LEXBOR
namespace {

std::string lexborString(const lxb_char_t *value, size_t length) {
  return value ? std::string(reinterpret_cast<const char *>(value), length)
               : std::string();
}

std::unique_ptr<ParsedNode> copyNode(lxb_dom_node_t *node) {
  auto result = std::make_unique<ParsedNode>();
  switch (node->type) {
  case LXB_DOM_NODE_TYPE_ELEMENT: {
    result->type = NodeType::Element;
    size_t nameLength = 0;
    const lxb_char_t *name = lxb_dom_node_name(node, &nameLength);
    result->name = lexborString(name, nameLength);

    auto *element = lxb_dom_interface_element(node);
    for (lxb_dom_attr_t *attribute = element->first_attr; attribute != nullptr;
         attribute = attribute->next) {
      size_t attributeNameLength = 0;
      size_t attributeValueLength = 0;
      const lxb_char_t *attributeName =
          lxb_dom_attr_qualified_name(attribute, &attributeNameLength);
      const lxb_char_t *attributeValue =
          lxb_dom_attr_value(attribute, &attributeValueLength);
      result->attributes.emplace_back(
          lexborString(attributeName, attributeNameLength),
          lexborString(attributeValue, attributeValueLength));
    }
    break;
  }
  case LXB_DOM_NODE_TYPE_TEXT:
    result->type = NodeType::Text;
    break;
  case LXB_DOM_NODE_TYPE_COMMENT:
    result->type = NodeType::Comment;
    break;
  case LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT:
    result->type = NodeType::DocumentFragment;
    break;
  default:
    return nullptr;
  }

  if (node->type == LXB_DOM_NODE_TYPE_TEXT ||
      node->type == LXB_DOM_NODE_TYPE_COMMENT) {
    size_t textLength = 0;
    const lxb_char_t *text = lxb_dom_node_text_content(node, &textLength);
    result->text = lexborString(text, textLength);
  }

  for (lxb_dom_node_t *child = node->first_child; child != nullptr;
       child = child->next) {
    auto copiedChild = copyNode(child);
    if (copiedChild) {
      result->children.push_back(std::move(copiedChild));
    }
  }
  return result;
}

} // namespace
#endif

std::unique_ptr<ParsedNode> parseHTMLTemplate(const std::string &html,
                                              std::string &error) {
#ifdef MYSTRAL_HAS_LEXBOR
  lxb_html_document_t *document = lxb_html_document_create();
  if (!document) {
    error = "Failed to create Lexbor HTML document";
    return nullptr;
  }

  static constexpr char emptyDocument[] =
      "<!doctype html><html><body></body></html>";
  lxb_status_t status = lxb_html_document_parse(
      document, reinterpret_cast<const lxb_char_t *>(emptyDocument),
      sizeof(emptyDocument) - 1);
  if (status != LXB_STATUS_OK) {
    error = "Failed to initialize Lexbor HTML document";
    lxb_html_document_destroy(document);
    return nullptr;
  }

  lxb_dom_element_t *context = lxb_dom_document_create_element(
      &document->dom_document, reinterpret_cast<const lxb_char_t *>("template"),
      8, nullptr);
  if (!context) {
    error = "Failed to create Lexbor template context";
    lxb_html_document_destroy(document);
    return nullptr;
  }

  lxb_dom_node_t *fragment = lxb_html_document_parse_fragment(
      document, context, reinterpret_cast<const lxb_char_t *>(html.data()),
      html.size());
  if (!fragment) {
    error = "Failed to parse HTML template fragment";
    lxb_html_document_destroy(document);
    return nullptr;
  }

  auto result = copyNode(fragment);
  lxb_html_document_destroy(document);
  if (!result) {
    error = "Failed to copy parsed HTML template tree";
  } else {
    // Lexbor's fragment parser returns its temporary context root. Expose that
    // root as the DocumentFragment represented by template.content.
    result->type = NodeType::DocumentFragment;
    result->name.clear();
    result->attributes.clear();
  }
  return result;
#else
  (void)html;
  error = "Lexbor HTML parsing is not enabled";
  return nullptr;
#endif
}

} // namespace mystral::dom
