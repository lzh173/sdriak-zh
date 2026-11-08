#pragma once

#include <gui/widgets/band_mapping.h>
#include <cassert>
#include <cstddef>
#include <string_view>
#include <vector>

namespace freq_input::band_groups {

    struct Group {
        std::string_view id;
        std::string_view label;
        BandServiceSet services;
    };

    struct Layout {
        std::vector<Group> groups;

        int indexOf(std::string_view id) const {
            for (std::size_t i = 0; i < groups.size(); i++) {
                if (groups[i].id == id) { return (int)i; }
            }
            return -1;
        }

        int indexFor(BandService service) const {
            for (std::size_t i = 0; i < groups.size(); i++) {
                if (groups[i].id != "all" &&
                    groups[i].services.contains(service))
                {
                    return (int)i;
                }
            }
            return indexOf("all");
        }
    };

    constexpr BandServiceSet withoutPrimaryServices() {
        return BandServiceSet::all()
            .without(BandService::Amateur)
            .without(BandService::Broadcast)
            .without(BandService::Aviation)
            .without(BandService::Maritime);
    }

    constexpr BandServiceSet withoutHamAndBroadcast() {
        return BandServiceSet::all()
            .without(BandService::Amateur)
            .without(BandService::Broadcast);
    }

    // The standard profile preserves the existing picker. The narrow profile
    // folds Air and Marine into Util when six usable segments do not fit.
    inline constexpr Group standardProfile[] = {
        { "ham", "业余", BandServiceSet::single(BandService::Amateur) },
        { "broadcast", "广播", BandServiceSet::single(BandService::Broadcast) },
        { "air", "航空", BandServiceSet::single(BandService::Aviation) },
        { "marine", "海事", BandServiceSet::single(BandService::Maritime) },
        { "utility", "其他", withoutPrimaryServices() }
    };

    inline constexpr Group narrowProfile[] = {
        { "ham", "业余", BandServiceSet::single(BandService::Amateur) },
        { "broadcast", "广播", BandServiceSet::single(BandService::Broadcast) },
        { "utility", "其他", withoutHamAndBroadcast() }
    };

    template <std::size_t N>
    Layout makeProfile(
        const Group (&profile)[N],
        BandServiceSet availableServices)
    {
        Layout layout;
        BandServiceSet covered;
        for (const Group& group : profile) {
            assert(!covered.intersects(group.services));
            covered = covered.merged(group.services);
            if (availableServices.intersects(group.services)) {
                layout.groups.push_back(group);
            }
        }
        assert(covered == BandServiceSet::all());
        layout.groups.push_back({ "all", "全部", BandServiceSet::all() });
        return layout;
    }

    inline Layout makeLayout(
        BandServiceSet availableServices,
        int maximumGroups)
    {
        Layout standard = makeProfile(standardProfile, availableServices);
        if ((int)standard.groups.size() <= maximumGroups) {
            return standard;
        }
        return makeProfile(narrowProfile, availableServices);
    }

}
