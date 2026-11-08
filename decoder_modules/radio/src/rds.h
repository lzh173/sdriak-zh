#pragma once
#include <stdint.h>
#include <string>
#include <chrono>
#include <mutex>

#define RDS_BLOCK_A_TIMEOUT_MS  5000.0
#define RDS_BLOCK_B_TIMEOUT_MS  5000.0
#define RDS_GROUP_0_TIMEOUT_MS  5000.0
#define RDS_GROUP_2_TIMEOUT_MS  5000.0
#define RDS_GROUP_10_TIMEOUT_MS 5000.0

namespace rds {
    enum BlockType {
        BLOCK_TYPE_A,
        BLOCK_TYPE_B,
        BLOCK_TYPE_C,
        BLOCK_TYPE_CP,
        BLOCK_TYPE_D,
        _BLOCK_TYPE_COUNT
    };

    enum GroupVersion {
        GROUP_VER_A,
        GROUP_VER_B
    };

    enum AreaCoverage {
        AREA_COVERAGE_INVALID           = -1,
        AREA_COVERAGE_LOCAL             = 0,
        AREA_COVERAGE_INTERNATIONAL     = 1,
        AREA_COVERAGE_NATIONAL          = 2,
        AREA_COVERAGE_SUPRA_NATIONAL    = 3,
        AREA_COVERAGE_REGIONAL1         = 4,
        AREA_COVERAGE_REGIONAL2         = 5,
        AREA_COVERAGE_REGIONAL3         = 6,
        AREA_COVERAGE_REGIONAL4         = 7,
        AREA_COVERAGE_REGIONAL5         = 8,
        AREA_COVERAGE_REGIONAL6         = 9,
        AREA_COVERAGE_REGIONAL7         = 10,
        AREA_COVERAGE_REGIONAL8         = 11,
        AREA_COVERAGE_REGIONAL9         = 12,
        AREA_COVERAGE_REGIONAL10        = 13,
        AREA_COVERAGE_REGIONAL11        = 14,
        AREA_COVERAGE_REGIONAL12        = 15  
    };

    inline const char* AREA_COVERAGE_TO_STR[] = {
        "本地",
        "国际",
        "全国",
        "跨国",
        "区域 1",
        "区域 2",
        "区域 3",
        "区域 4",
        "区域 5",
        "区域 6",
        "区域 7",
        "区域 8",
        "区域 9",
        "区域 10",
        "区域 11",
        "区域 12",
    };

    enum ProgramType {
        // US Types
        PROGRAM_TYPE_US_NONE = 0,
        PROGRAM_TYPE_US_NEWS = 1,
        PROGRAM_TYPE_US_INFOMATION = 2,
        PROGRAM_TYPE_US_SPORTS = 3,
        PROGRAM_TYPE_US_TALK = 4,
        PROGRAM_TYPE_US_ROCK = 5,
        PROGRAM_TYPE_US_CLASSIC_ROCK = 6,
        PROGRAM_TYPE_US_ADULT_HITS = 7,
        PROGRAM_TYPE_US_SOFT_ROCK = 8,
        PROGRAM_TYPE_US_TOP_40 = 9,
        PROGRAM_TYPE_US_COUNTRY = 10,
        PROGRAM_TYPE_US_OLDIES = 11,
        PROGRAM_TYPE_US_SOFT = 12,
        PROGRAM_TYPE_US_NOSTALGIA = 13,
        PROGRAM_TYPE_US_JAZZ = 14,
        PROGRAM_TYPE_US_CLASSICAL = 15,
        PROGRAM_TYPE_US_RHYTHM_AND_BLUES = 16,
        PROGRAM_TYPE_US_SOFT_RHYTHM_AND_BLUES = 17,
        PROGRAM_TYPE_US_FOREIGN_LANGUAGE = 18,
        PROGRAM_TYPE_US_RELIGIOUS_MUSIC = 19,
        PROGRAM_TYPE_US_RELIGIOUS_TALK = 20,
        PROGRAM_TYPE_US_PERSONALITY = 21,
        PROGRAM_TYPE_US_PUBLIC = 22,
        PROGRAM_TYPE_US_COLLEGE = 23,
        PROGRAM_TYPE_US_UNASSIGNED0 = 24,
        PROGRAM_TYPE_US_UNASSIGNED1 = 25,
        PROGRAM_TYPE_US_UNASSIGNED2 = 26,
        PROGRAM_TYPE_US_UNASSIGNED3 = 27,
        PROGRAM_TYPE_US_UNASSIGNED4 = 28,
        PROGRAM_TYPE_US_WEATHER = 29,
        PROGRAM_TYPE_US_EMERGENCY_TEST = 30,
        PROGRAM_TYPE_US_EMERGENCY = 31,

        // EU Types
        PROGRAM_TYPE_EU_NONE = 0,
        PROGRAM_TYPE_EU_NEWS = 1,
        PROGRAM_TYPE_EU_CURRENT_AFFAIRS = 2,
        PROGRAM_TYPE_EU_INFORMATION = 3,
        PROGRAM_TYPE_EU_SPORTS = 4,
        PROGRAM_TYPE_EU_EDUCATION = 5,
        PROGRAM_TYPE_EU_DRAMA = 6,
        PROGRAM_TYPE_EU_CULTURE = 7,
        PROGRAM_TYPE_EU_SCIENCE = 8,
        PROGRAM_TYPE_EU_VARIED = 9,
        PROGRAM_TYPE_EU_POP_MUSIC = 10,
        PROGRAM_TYPE_EU_ROCK_MUSIC = 11,
        PROGRAM_TYPE_EU_EASY_LISTENING_MUSIC = 12,
        PROGRAM_TYPE_EU_LIGHT_CLASSICAL = 13,
        PROGRAM_TYPE_EU_SERIOUS_CLASSICAL = 14,
        PROGRAM_TYPE_EU_OTHER_MUSIC = 15,
        PROGRAM_TYPE_EU_WEATHER = 16,
        PROGRAM_TYPE_EU_FINANCE = 17,
        PROGRAM_TYPE_EU_CHILDRENS_PROGRAM = 18,
        PROGRAM_TYPE_EU_SOCIAL_AFFAIRS = 19,
        PROGRAM_TYPE_EU_RELIGION = 20,
        PROGRAM_TYPE_EU_PHONE_IN = 21,
        PROGRAM_TYPE_EU_TRAVEL = 22,
        PROGRAM_TYPE_EU_LEISURE = 23,
        PROGRAM_TYPE_EU_JAZZ_MUSIC = 24,
        PROGRAM_TYPE_EU_COUNTRY_MUSIC = 25,
        PROGRAM_TYPE_EU_NATIONAL_MUSIC = 26,
        PROGRAM_TYPE_EU_OLDIES_MUSIC = 27,
        PROGRAM_TYPE_EU_FOLK_MUSIC = 28,
        PROGRAM_TYPE_EU_DOCUMENTARY = 29,
        PROGRAM_TYPE_EU_ALARM_TEST = 30,
        PROGRAM_TYPE_EU_ALARM = 31
    };

    inline const char* PROGRAM_TYPE_EU_TO_STR[] = {
        "无",
        "新闻",
        "时事",
        "信息",
        "体育",
        "教育",
        "戏剧",
        "文化",
        "科学",
        "综合",
        "流行音乐",
        "摇滚乐",
        "轻音乐",
        "轻古典",
        "严肃古典",
        "其他音乐",
        "天气",
        "财经",
        "儿童节目",
        "社会事务",
        "宗教",
        "热线参与",
        "旅游",
        "休闲",
        "爵士乐",
        "乡村音乐",
        "民族音乐",
        "怀旧音乐",
        "民谣音乐",
        "纪录片",
        "警报测试",
        "警报",
    };

    inline const char* PROGRAM_TYPE_US_TO_STR[] = {
        "无",
        "新闻",
        "信息",
        "体育",
        "谈话",
        "摇滚",
        "经典摇滚",
        "成人热门",
        "轻柔摇滚",
        "Top 40",
        "乡村",
        "怀旧",
        "轻柔",
        "怀旧金曲",
        "爵士",
        "古典",
        "节奏蓝调",
        "轻柔节奏蓝调",
        "外语",
        "宗教音乐",
        "宗教谈话",
        "个性",
        "公共",
        "校园",
        "未分配",
        "未分配",
        "未分配",
        "未分配",
        "未分配",
        "天气",
        "应急测试",
        "应急",
    };

    enum DecoderIdentification {
        DECODER_IDENT_STEREO = (1 << 0),
        DECODER_IDENT_ARTIFICIAL_HEAD = (1 << 1),
        DECODER_IDENT_COMPRESSED = (1 << 2),
        DECODER_IDENT_VARIABLE_PTY = (1 << 0)
    };

    class Decoder {
    public:
        void process(uint8_t* symbols, int count);

        bool piCodeValid() { std::lock_guard<std::mutex> lck(blockAMtx); return blockAValid(); }
        uint16_t getPICode() { std::lock_guard<std::mutex> lck(blockAMtx); return piCode; }
        uint8_t getCountryCode() { std::lock_guard<std::mutex> lck(blockAMtx); return countryCode; }
        uint8_t getProgramCoverage() { std::lock_guard<std::mutex> lck(blockAMtx); return programCoverage; }
        uint8_t getProgramRefNumber() { std::lock_guard<std::mutex> lck(blockAMtx); return programRefNumber; }
        std::string getCallsign() { std::lock_guard<std::mutex> lck(blockAMtx); return callsign; }
        
        bool programTypeValid() { std::lock_guard<std::mutex> lck(blockBMtx); return blockBValid(); }
        ProgramType getProgramType() { std::lock_guard<std::mutex> lck(blockBMtx); return programType; }

        bool musicValid() { std::lock_guard<std::mutex> lck(group0Mtx); return group0Valid(); }
        bool getMusic() { std::lock_guard<std::mutex> lck(group0Mtx); return music; }
        bool PSNameValid() { std::lock_guard<std::mutex> lck(group0Mtx); return group0Valid(); }
        std::string getPSName(bool incremental) { std::lock_guard<std::mutex> lck(group0Mtx); return repertoireToUtf8(incremental ? programServiceName : programServiceNameFullUpdate); }

        bool radioTextValid() { std::lock_guard<std::mutex> lck(group2Mtx); return group2Valid(); }
        std::string getRadioText(bool incremental) { std::lock_guard<std::mutex> lck(group2Mtx); return repertoireToUtf8(incremental ? radioText : radioTextFullUpdate); }

        bool programTypeNameValid() { std::lock_guard<std::mutex> lck(group10Mtx); return group10Valid(); }
        std::string getProgramTypeName() { std::lock_guard<std::mutex> lck(group10Mtx); return repertoireToUtf8(programTypeName); }

    private:
        static uint16_t calcSyndrome(uint32_t block);
        static uint32_t correctErrors(uint32_t block, BlockType type, bool& recovered);
        void decodeBlockA();
        void decodeBlockB();
        void decodeGroup0();
        void decodeGroup2();
        void decodeGroup10();
        void decodeGroup();

        static std::string base26ToCall(uint16_t pi);
        static std::string decodeCallsign(uint16_t pi);

        // Translate raw RDS bytes (EBU G0 repertoire) into a UTF-8 string
        static std::string repertoireToUtf8(const std::string& raw);

        bool blockAValid();
        bool blockBValid();
        bool group0Valid();
        bool group2Valid();
        bool group10Valid();

        // State machine
        uint32_t shiftReg = 0;
        int sync = 0;
        int skip = 0;
        BlockType lastType = BLOCK_TYPE_A;
        int contGroup = 0;
        uint32_t blocks[_BLOCK_TYPE_COUNT];
        bool blockAvail[_BLOCK_TYPE_COUNT];

        // Block A (All groups)
        std::mutex blockAMtx;
        std::chrono::time_point<std::chrono::high_resolution_clock> blockALastUpdate{};  // 1970-01-01
        uint16_t piCode;
        uint8_t countryCode;
        AreaCoverage programCoverage;
        uint8_t programRefNumber;
        std::string callsign;

        // Block B (All groups)
        std::mutex blockBMtx;
        std::chrono::time_point<std::chrono::high_resolution_clock> blockBLastUpdate{};  // 1970-01-01
        uint8_t groupType;
        GroupVersion groupVer;
        bool trafficProgram;
        ProgramType programType;

        // Group type 0
        std::mutex group0Mtx;
        std::chrono::time_point<std::chrono::high_resolution_clock> group0LastUpdate{};  // 1970-01-01
        bool trafficAnnouncement;
        bool music;
        uint8_t decoderIdent;
        uint16_t alternateFrequency;
        std::string programServiceName = "        ";
        std::string programServiceNameFullUpdate = "        ";

        // Group type 2
        std::mutex group2Mtx;
        std::chrono::time_point<std::chrono::high_resolution_clock> group2LastUpdate{};  // 1970-01-01
        bool rtAB = false;
        std::string radioText = "                                                                ";
        std::string radioTextFullUpdate = "                                                                ";

        // Group type 10
        std::mutex group10Mtx;
        std::chrono::time_point<std::chrono::high_resolution_clock> group10LastUpdate{};  // 1970-01-01
        bool ptnAB = false;
        std::string programTypeName = "        ";

    };
}