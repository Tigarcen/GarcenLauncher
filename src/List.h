#pragma once

#include <string>
#include <vector>

// ========================================
// Item
// ========================================

struct Item
{
    std::wstring name;
    std::wstring path;
    std::vector<Item> children;
};

// ========================================
// Public API
// ========================================

// Ensure list.json exists next to the executable (create with [] when
// missing), then read and parse it into a vector of Item
std::vector<Item> InitList();

// Serialize the given items back to list.json next to the executable;
// returns true on success
bool UpdateList(const std::vector<Item>& items);
