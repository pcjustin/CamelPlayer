#include "OutputBridge/AsioOutput.h"
#include "OutputBridge/KsOutput.h"
#include "OutputBridge/WasapiOutput.h"

namespace camel::audio {

std::unique_ptr<IOutput> createOutput(const Settings& s) {
    switch (s.backend) {
    case Backend::Wasapi:
        return createWasapiOutput(s.wasapi);
    case Backend::Asio:
        return createAsioOutput(s.asio);
    case Backend::Ks:
        return createKsOutput(s.ks);
    }
    return createWasapiOutput(s.wasapi);
}

} // namespace camel::audio
