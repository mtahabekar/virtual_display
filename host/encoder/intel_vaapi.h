#pragma once
#include "capture/capture.h"
#include <string>
namespace quest {
class IntelVaapi : public DmaBufConverter {
public:
    explicit IntelVaapi(const std::string &device);
    ~IntelVaapi();
    AVBufferRef *frames() const;
    std::shared_ptr<AVFrame> allocate() override;
    std::vector<uint64_t> modifiers() const override;
    void import(spa_buffer *, uint64_t, bool) override;
    void convert(spa_buffer *, AVFrame *) override;
    void remove(spa_buffer *) override;
    void settle() override;
    std::shared_ptr<AVFrame> upload(const MappedFrame &);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
