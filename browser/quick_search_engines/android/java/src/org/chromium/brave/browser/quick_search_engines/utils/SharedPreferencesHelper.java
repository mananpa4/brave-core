/* Copyright (c) 2024 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

package org.chromium.brave.browser.quick_search_engines.utils;

import com.google.gson.GsonBuilder;
import com.google.gson.JsonParseException;
import com.google.gson.reflect.TypeToken;

import org.chromium.base.Log;
import org.chromium.chrome.browser.preferences.ChromeSharedPreferences;

import java.util.Map;

public class SharedPreferencesHelper {
    private static final String TAG = "SharedPrefsHelper";

    public <K, V> void saveMap(String key, Map<K, V> map) {
        String json = new GsonBuilder().create().toJson(map);
        if (json != null && !json.isEmpty()) {
            ChromeSharedPreferences.getInstance().writeString(key, json);
        }
    }

    /** Returns null when nothing is stored, or when the stored JSON cannot be parsed. */
    public <K, V> Map<K, V> getMap(String key, Class keyClass, Class valueClass) {
        String json = ChromeSharedPreferences.getInstance().readString(key, "");
        if (json.isEmpty()) {
            return null;
        }
        try {
            return new GsonBuilder()
                    .create()
                    .fromJson(
                            json,
                            TypeToken.getParameterized(Map.class, keyClass, valueClass).getType());
        } catch (JsonParseException e) {
            // Data written by a build that serialized this type differently, or otherwise
            // corrupted. Drop it so the caller falls back to its defaults instead of crashing.
            Log.e(TAG, "Discarding unparsable value for %s", key, e);
            ChromeSharedPreferences.getInstance().removeKey(key);
            return null;
        }
    }
}
