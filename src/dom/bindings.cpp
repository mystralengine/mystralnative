#include "mystral/dom/bindings.h"

#include "mystral/dom/html_template.h"
#include "mystral/js/engine.h"

#include <iostream>
#include <string>

namespace mystral::dom {

namespace {

js::Engine *g_engine = nullptr;
bool g_debug = false;

js::JSValueHandle createJSNode(const ParsedNode &node,
                               js::JSValueHandle parent) {
  auto result = g_engine->newObject();
  g_engine->setProperty(result, "nodeType",
                        g_engine->newNumber(static_cast<int>(node.type)));
  g_engine->setProperty(result, "parentNode", parent);

  if (node.type == NodeType::Element) {
    std::string localName = node.name;
    for (char &character : localName) {
      if (character >= 'A' && character <= 'Z') {
        character = static_cast<char>(character - 'A' + 'a');
      }
    }
    g_engine->setProperty(result, "nodeName",
                          g_engine->newString(node.name.c_str()));
    g_engine->setProperty(result, "tagName",
                          g_engine->newString(node.name.c_str()));
    g_engine->setProperty(result, "localName",
                          g_engine->newString(localName.c_str()));

    auto attributes = g_engine->newObject();
    for (const auto &[name, value] : node.attributes) {
      g_engine->setProperty(attributes, name.c_str(),
                            g_engine->newString(value.c_str()));
      g_engine->setProperty(result, name.c_str(),
                            g_engine->newString(value.c_str()));
      if (name == "class") {
        g_engine->setProperty(result, "className",
                              g_engine->newString(value.c_str()));
      }
    }
    g_engine->setProperty(result, "attributes", attributes);
    g_engine->setProperty(result, "style", g_engine->newObject());
    g_engine->setProperty(result, "dataset", g_engine->newObject());
  } else if (node.type == NodeType::Text || node.type == NodeType::Comment) {
    const char *nodeName = node.type == NodeType::Text ? "#text" : "#comment";
    g_engine->setProperty(result, "nodeName", g_engine->newString(nodeName));
    g_engine->setProperty(result, "data",
                          g_engine->newString(node.text.c_str()));
    g_engine->setProperty(result, "nodeValue",
                          g_engine->newString(node.text.c_str()));
    g_engine->setProperty(result, "textContent",
                          g_engine->newString(node.text.c_str()));
  } else {
    g_engine->setProperty(result, "nodeName",
                          g_engine->newString("#document-fragment"));
  }

  auto childNodes = g_engine->newArray(node.children.size());
  auto children = g_engine->newArray();
  uint32_t elementIndex = 0;
  for (uint32_t index = 0; index < node.children.size(); ++index) {
    auto child = createJSNode(*node.children[index], result);
    g_engine->setPropertyIndex(childNodes, index, child);
    if (node.children[index]->type == NodeType::Element) {
      g_engine->setPropertyIndex(children, elementIndex++, child);
    }
  }
  g_engine->setProperty(result, "childNodes", childNodes);
  g_engine->setProperty(result, "children", children);
  g_engine->setProperty(result, "childElementCount",
                        g_engine->newNumber(elementIndex));
  return result;
}

} // namespace

bool initBindings(js::Engine *engine, bool debug) {
  if (!engine) {
    return false;
  }
  g_engine = engine;
  g_debug = debug;

  engine->setGlobalProperty(
      "__parseHTMLTemplate",
      engine->newFunction(
          "__parseHTMLTemplate",
          [](void *, const std::vector<js::JSValueHandle> &args) {
            const std::string html =
                args.empty() ? "" : g_engine->toString(args[0]);
            std::string error;
            auto tree = parseHTMLTemplate(html, error);
            if (!tree) {
              g_engine->throwException(error.c_str());
              return g_engine->newNull();
            }

            auto root = createJSNode(*tree, g_engine->newNull());
            auto hydrate =
                g_engine->getGlobalProperty("__mystralHydrateDOMTree");
            if (g_engine->isFunction(hydrate)) {
              return g_engine->call(hydrate, g_engine->newUndefined(), {root});
            }
            return root;
          }));

  const bool initialized = engine->evalScript(R"JS(
if (typeof globalThis.Node === 'undefined') globalThis.Node = class Node {};
if (typeof globalThis.Element === 'undefined') globalThis.Element = class Element extends Node {};
if (typeof globalThis.HTMLElement === 'undefined') globalThis.HTMLElement = class HTMLElement extends Element {};
if (typeof globalThis.Text === 'undefined') globalThis.Text = class Text extends Node {};
if (typeof globalThis.Comment === 'undefined') globalThis.Comment = class Comment extends Node {};
if (typeof globalThis.DocumentFragment === 'undefined') globalThis.DocumentFragment = class DocumentFragment extends Node {};

const nodePrototypeFor = node => {
    if (node.nodeType === 1) return HTMLElement.prototype;
    if (node.nodeType === 3) return Text.prototype;
    if (node.nodeType === 8) return Comment.prototype;
    return DocumentFragment.prototype;
};

const cloneDOMNode = (node, deep = false) => {
    const clone = Object.create(nodePrototypeFor(node));
    for (const key of ['nodeType', 'nodeName', 'tagName', 'localName', 'data', 'nodeValue', 'textContent', 'id', 'className']) {
        if (key in node) clone[key] = node[key];
    }
    clone.attributes = { ...(node.attributes || {}) };
    clone.style = { ...(node.style || {}) };
    clone.dataset = { ...(node.dataset || {}) };
    clone.childNodes = [];
    clone.children = [];
    clone.parentNode = null;
    if (deep) {
        for (const child of node.childNodes || []) {
            const childClone = cloneDOMNode(child, true);
            childClone.parentNode = clone;
            clone.childNodes.push(childClone);
            if (childClone.nodeType === 1) clone.children.push(childClone);
        }
    }
    return installDOMMethods(clone);
};

const installDOMMethods = node => {
    Object.setPrototypeOf(node, nodePrototypeFor(node));
    node.childNodes ||= [];
    node.children ||= [];
    node.appendChild = function(child) {
        if (!child) return child;
        child.parentNode?.removeChild?.(child);
        child.parentNode = this;
        this.childNodes.push(child);
        if (child.nodeType === 1) this.children.push(child);
        return child;
    };
    node.append = function(...children) { for (const child of children) this.appendChild(child); };
    node.insertBefore = function(child, reference) {
        if (!reference) return this.appendChild(child);
        const index = this.childNodes.indexOf(reference);
        if (index < 0) return this.appendChild(child);
        child.parentNode?.removeChild?.(child);
        child.parentNode = this;
        this.childNodes.splice(index, 0, child);
        if (child.nodeType === 1) {
            const elementIndex = this.childNodes.slice(0, index).filter(node => node.nodeType === 1).length;
            this.children.splice(elementIndex, 0, child);
        }
        return child;
    };
    node.removeChild = function(child) {
        this.childNodes = this.childNodes.filter(value => value !== child);
        this.children = this.children.filter(value => value !== child);
        if (child) child.parentNode = null;
        return child;
    };
    node.remove = function() { this.parentNode?.removeChild?.(this); };
    node.before = function(...nodes) {
        if (!this.parentNode) return;
        for (const value of nodes) this.parentNode.insertBefore(value, this);
    };
    node.after = function(...nodes) {
        if (!this.parentNode) return;
        const siblings = this.parentNode.childNodes;
        let reference = siblings[siblings.indexOf(this) + 1] || null;
        for (const value of nodes) {
            this.parentNode.insertBefore(value, reference);
            reference = siblings[siblings.indexOf(value) + 1] || null;
        }
    };
    node.replaceWith = function(...nodes) {
        if (!this.parentNode) return;
        const parent = this.parentNode;
        for (const value of nodes) parent.insertBefore(value, this);
        parent.removeChild(this);
    };
    node.cloneNode = function(deep = false) { return cloneDOMNode(this, deep); };
    if (node.nodeType === 1) {
        node.setAttribute = function(name, value) { this.attributes[name] = String(value); this[name] = String(value); };
        node.getAttribute = function(name) { return this.attributes[name] ?? null; };
        node.removeAttribute = function(name) { delete this.attributes[name]; delete this[name]; };
    }
    for (const child of node.childNodes) {
        installDOMMethods(child);
        child.parentNode = node;
    }
    node.children = node.childNodes.filter(child => child.nodeType === 1);
    return node;
};

globalThis.__mystralHydrateDOMTree = installDOMMethods;

if (typeof globalThis.HTMLTemplateElement === 'undefined') {
    globalThis.HTMLTemplateElement = class HTMLTemplateElement extends HTMLElement {
        get innerHTML() { return this.__innerHTML || ''; }
        set innerHTML(value) {
            this.__innerHTML = String(value);
            this.content = __parseHTMLTemplate(this.__innerHTML);
        }
    };
}
globalThis.__mystralSetTemplatePrototype = value => {
    Object.setPrototypeOf(value, HTMLTemplateElement.prototype);
    value.content ||= __parseHTMLTemplate('');
    return value;
};
)JS",
                                              "<dom-bindings>");

  if (g_debug && initialized) {
    std::cout << "[DOM] Lexbor HTML template bindings initialized" << std::endl;
  }
  return initialized;
}

} // namespace mystral::dom
