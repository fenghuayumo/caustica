#pragma once

#include <shaders/PathTracer/Config.h>
//#include "../common/SampleCommon.h"
#include <scene/GameTypes.h>

namespace caustica
{
    class PlanarView;
}

namespace game
{
    // Sample/demo gameplay *script* attached to a PropBase - NOT an engine ECS component.
    // Scene truth lives in SceneEntityWorld; scripts only Tick / drive UI over that ECS.
    // Prefer caustica::spawn / SceneMeshEdit / SceneTransform for mesh and transform edits.
    class PropComponentBase
    {
    public:
        PropComponentBase(class PropBase & prop, const std::string & type);
        virtual ~PropComponentBase() { }

        virtual void            Tick(double gameTime, float animationTime, float deltaTime) = 0;
        virtual ScreenGUISel    StandaloneGUI(const std::shared_ptr<caustica::PlanarView> & view, const float2 & mousePos, const float2 & displaySize) { return ScreenGUISel{}; }

        static std::shared_ptr<PropComponentBase> create(class PropBase & prop, const Json::Value & loadData);

    protected:
        virtual void            load(const Json::Value & loadData) { }

    protected:
        PropBase &              m_prop;
        std::string             m_type;
    };
}
