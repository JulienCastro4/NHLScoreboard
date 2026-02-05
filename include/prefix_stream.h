#pragma once

#include <Arduino.h>

class PrefixStream : public Stream {
public:
    PrefixStream(Stream& base, char first)
        : base_(base), hasPrefix_(true), prefix_(first) {}

    int available() override {
        return (hasPrefix_ ? 1 : 0) + base_.available();
    }

    int read() override {
        if (hasPrefix_) {
            hasPrefix_ = false;
            return (int)prefix_;
        }
        return base_.read();
    }

    int peek() override {
        if (hasPrefix_)
            return (int)prefix_;
        return base_.peek();
    }

    void flush() override {
        base_.flush();
    }

    size_t write(uint8_t b) override {
        return base_.write(b);
    }

    size_t readBytes(char* buffer, size_t length) override {
        size_t n = 0;
        if (hasPrefix_ && length > 0) {
            buffer[0] = prefix_;
            hasPrefix_ = false;
            n = 1;
        }
        return n + base_.readBytes(buffer + n, length - n);
    }

private:
    Stream& base_;
    bool hasPrefix_;
    char prefix_;
};

