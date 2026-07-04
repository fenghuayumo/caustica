#pragma once

namespace caustica
{

class EngineBuilder;

// Static plugin entry point (Bevy-style). Plugins compose subsystems and optional
// frame-pass extensions at compile time; no DLL loading required.
//
// Example:
//   struct MyPlugin : IEnginePlugin {
//       void build(EngineBuilder& builder) override {
//           builder.addPlugin<DefaultRuntimePlugin>();
//           // builder.registerFramePass<MyCustomPass>(...);
//       }
//   };
class IEnginePlugin
{
public:
    virtual ~IEnginePlugin() = default;

    // Register subsystems, pass hooks, and other engine extensions.
    virtual void build(EngineBuilder& builder) = 0;
};

} // namespace caustica
