#pragma once

namespace CallLogger {
    void Init();          // attach the call sink (no-op standalone)
    void Shutdown();
    void Render();        // dockable tab next to the Node Editor
}
