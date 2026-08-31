#pragma once

namespace mystral::js {
class Engine;
}

namespace mystral::dom {

bool initBindings(js::Engine *engine, bool debug = false);

} // namespace mystral::dom
