#pragma once
namespace Independent
{
    void Start();
    bool Active();
    void Frame();
    void LoadStart();
    void LoadProgress();
    void LoadEnd();
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
