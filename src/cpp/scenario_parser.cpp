#include "cse/scenario_parser.hpp"

#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>


namespace cse
{

namespace
{

std::pair<cse::simulator_index, cse::simulator*> find_simulator(
    const std::unordered_map<simulator_index, simulator*>& simulators,
    const std::string& model)
{
    for (const auto& [idx, simulator] : simulators) {
        if (simulator->name() == model) {
            return std::make_pair(idx, simulator);
        }
    }
    throw std::invalid_argument("Can't find model with this name");
}

cse::variable_type find_variable_type(const nlohmann::json& j)
{
    auto typestr = j.get<std::string>();
    if (typestr == "real") {
        return variable_type::real;
    } else if (typestr == "integer") {
        return variable_type::integer;
    } else if (typestr == "boolean") {
        return variable_type::boolean;
    } else if (typestr == "string") {
        return variable_type::string;
    }
    throw std::invalid_argument("Can't process unknown variable type");
}

cse::variable_causality find_causality(const nlohmann::json& j)
{
    auto caus = j.get<std::string>();
    if (caus == "output") {
        return variable_causality::output;
    } else if (caus == "input") {
        return variable_causality::input;
    } else if (caus == "parameter") {
        return variable_causality::parameter;
    } else if (caus == "calculatedParameter") {
        return variable_causality::calculated_parameter;
    } else if (caus == "local") {
        return variable_causality::local;
    }
    throw std::invalid_argument("Can't process unknown variable type");
}

bool is_input(cse::variable_causality causality)
{
    switch (causality) {
        case input:
        case parameter:
            return true;
        case calculated_parameter:
        case output:
            return false;
        default:
            throw std::invalid_argument(
                "No support for manipulating a variable with this causality");
    }
}

cse::variable_index find_variable_index(
    const std::vector<variable_description>& variables,
    const std::string& name,
    const cse::variable_type type,
    const cse::variable_causality causality)
{
    for (const auto& vd : variables) {
        if ((vd.name == name) && (vd.type == type) && (vd.causality == causality)) {
            return vd.index;
        }
    }
    throw std::invalid_argument("Can't find variable index");
}

template<typename T>
std::function<T(T)> generate_manipulator(
    const std::string& kind,
    const nlohmann::json& event)
{
    if ("reset" == kind) {
        return nullptr;
    }
    T value = event.at("value").get<T>();
    if ("bias" == kind) {
        return [value](T original) { return original + value; };
    } else if ("override" == kind) {
        return [value](T /*original*/) { return value; };
    }
    throw std::invalid_argument("Can't process unknown modifier kind");
}

cse::scenario::variable_action generate_action(
    const nlohmann::json& event,
    const std::string& mode,
    cse::simulator_index sim,
    cse::variable_type type,
    bool isInput,
    cse::variable_index var)
{
    switch (type) {
        case cse::variable_type::real: {
            auto f = generate_manipulator<double>(mode, event);
            return cse::scenario::variable_action{
                sim, var, cse::scenario::real_manipulator{f}, isInput};
        }
        case cse::variable_type::integer: {
            auto f = generate_manipulator<int>(mode, event);
            return cse::scenario::variable_action{
                sim, var, cse::scenario::integer_manipulator{f}, isInput};
        }
        default:
            throw std::invalid_argument("No support for this variable type");
    }
}

struct defaults
{
    std::optional<std::string> model;
    std::optional<std::string> variable;
    std::optional<std::string> causality;
    std::optional<std::string> type;
    std::optional<std::string> action;
};

std::optional<std::string> parse_element(
    const nlohmann::json& j,
    const std::string& name)
{
    if (j.count(name)) {
        return j.at(name).get<std::string>();
    } else {
        return std::nullopt;
    }
}

defaults parse_defaults(const nlohmann::json& scenario)
{
    if (scenario.count("defaults")) {
        auto j = scenario.at("defaults");
        return defaults{
            parse_element(j, "model"),
            parse_element(j, "variable"),
            parse_element(j, "causality"),
            parse_element(j, "type"),
            parse_element(j, "action")};
    }
    return defaults{};
}

std::string specified_or_default(
    const nlohmann::json& j,
    const std::string& name,
    std::optional<std::string> defaultOption)
{
    if (j.count(name)) {
        return j.at(name).get<std::string>();
    } else if (defaultOption.has_value()) {
        return *defaultOption;
    }
    throw std::invalid_argument("Option is not specified explicitly nor in defaults");
}

std::optional<cse::time_point> parse_end_time(const nlohmann::json& j)
{
    if (j.count("end")) {
        auto endTime = j.at("end").get<double>();
        return to_time_point(endTime);
    }
    return std::nullopt;
}

} // namespace


scenario::scenario parse_scenario(
    const boost::filesystem::path& scenarioFile,
    const std::unordered_map<simulator_index,
        simulator*>& simulators)
{
    boost::filesystem::ifstream i(scenarioFile);
    nlohmann::json j;
    i >> j;

    std::vector<scenario::event> events;
    defaults defaultOpts = parse_defaults(j);

    for (auto& event : j.at("events")) {
        auto trigger = event.at("time");
        auto triggerTime = trigger.get<double>();
        auto time = to_time_point(triggerTime);

        const auto& [index, simulator] =
            find_simulator(simulators, specified_or_default(event, "model", defaultOpts.model));
        variable_type type =
            find_variable_type(specified_or_default(event, "type", defaultOpts.type));
        variable_causality causality =
            find_causality(specified_or_default(event, "causality", defaultOpts.causality));
        auto varName =
            specified_or_default(event, "variable", defaultOpts.variable);
        variable_index varIndex =
            find_variable_index(simulator->model_description().variables, varName, type, causality);

        auto mode = specified_or_default(event, "action", defaultOpts.action);
        bool isInput = is_input(causality);
        scenario::variable_action a = generate_action(event, mode, index, type, isInput, varIndex);
        events.emplace_back(scenario::event{time, a});
    }

    auto end = parse_end_time(j);
    return scenario::scenario{events, end};
}
} // namespace cse