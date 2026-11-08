#pragma once
#include <sched_action.h>

namespace sched_action {
    class StartRecorderClass : public ActionClass {
    public:
        StartRecorderClass() {}
        ~StartRecorderClass() {}

        void trigger() {
        }

        void prepareEditMenu() {
        }

        bool showEditMenu(bool& valid) {
            valid = false;
            return false;
        }

        void loadFromConfig(json config) {
            if (config.contains("recorder")) { recorderName = config["recorder"]; }
            name = "启动 \"" + recorderName + "\"";
        }

        json saveToConfig() {
            json config;
            config["recorder"] = recorderName;
            return config;
        }


        std::string getName() {
            return name;
        }

    private:
        std::string recorderName;

        std::string name = "启动 \"\"";
    };

    Action StartRecorder() {
        return Action(new StartRecorderClass);
    }
}