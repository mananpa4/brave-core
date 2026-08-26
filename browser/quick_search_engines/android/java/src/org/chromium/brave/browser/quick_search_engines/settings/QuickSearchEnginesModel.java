/* Copyright (c) 2024 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

package org.chromium.brave.browser.quick_search_engines.settings;

import androidx.annotation.IntDef;

import com.google.gson.annotations.SerializedName;

/**
 * Persisted to shared preferences as JSON by {@link
 * org.chromium.brave.browser.quick_search_engines.utils.SharedPreferencesHelper}. Every persisted
 * field needs an explicit {@link SerializedName}: without one Gson derives the JSON key from the
 * field name, which R8 rewrites, so the stored format would differ between builds.
 */
public class QuickSearchEnginesModel {
    @IntDef({
        QuickSearchEnginesModelType.SEARCH_ENGINE,
        QuickSearchEnginesModelType.AI_ASSISTANT,
    })
    public @interface QuickSearchEnginesModelType {
        int SEARCH_ENGINE = 0;
        int AI_ASSISTANT = 1;
    }

    @SerializedName("shortName")
    private final String mShortName;

    @SerializedName("keyword")
    private final String mKeyword;

    @SerializedName("url")
    private final String mUrl;

    @SerializedName("isEnabled")
    private boolean mIsEnabled;

    @SerializedName("type")
    private final @QuickSearchEnginesModelType int mType;

    public QuickSearchEnginesModel(
            String shortName,
            String keyword,
            String url,
            boolean isEnabled,
            @QuickSearchEnginesModelType int type) {
        mShortName = shortName;
        mKeyword = keyword;
        mUrl = url;
        mIsEnabled = isEnabled;
        mType = type;
    }

    public String getShortName() {
        return mShortName;
    }

    public String getKeyword() {
        return mKeyword;
    }

    public String getUrl() {
        return mUrl;
    }

    public boolean isEnabled() {
        return mIsEnabled;
    }

    public void setEnabled(boolean isEnabled) {
        mIsEnabled = isEnabled;
    }

    public @QuickSearchEnginesModelType int getType() {
        return mType;
    }
}
