#pragma once
namespace Independent
{
    void Start();
    void GameReady(); // called after game-dependent initialization
    bool Active();
    void Frame();
    void LoadStart();
    void LoadProgress();
    void LoadEnd();
    // (12) The main thread is compiling shaders right now: keeps the helper from treating the pause as a freeze
    void ShaderCompiling();
    bool PendingManual();
    bool PendingAutomatic();
    void Acknowledge(bool manual);
    class Guard {
    public:
        explicit Guard(bool capture = false);
        ~Guard();
        explicit operator bool() const { return held_; }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        void* shared_ = nullptr;
        bool held_ = false;
        bool capture_ = false;
    };
}
