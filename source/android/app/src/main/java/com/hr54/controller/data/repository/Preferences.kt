package com.hr54.controller.data.repository

import android.content.Context
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.map

private val Context.preferences by preferencesDataStore("receiver")

class ReceiverPreferences(context: Context) {
    private val store = context.preferences
    private val key = stringPreferencesKey("base_url")
    val receiver = store.data.map { it[key] }

    suspend fun save(value: String) {
        store.edit { it[key] = value }
    }
}
