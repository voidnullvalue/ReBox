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

interface ManagementTokenStore {
    suspend fun read(receiver: String): String?
    suspend fun write(receiver: String, token: String?)
}

/** No-backup, app-private storage. A receiver address never becomes a path. */
class PrivateManagementTokens(context: Context) : ManagementTokenStore {
    private val directory = java.io.File(context.noBackupFilesDir, "module-tokens")
    private fun file(receiver: String): android.util.AtomicFile {
        directory.mkdirs()
        android.system.Os.chmod(directory.path, 448) // 0700
        val key = java.security.MessageDigest.getInstance("SHA-256").digest(receiver.toByteArray())
            .joinToString("") { "%02x".format(it) }
        return android.util.AtomicFile(java.io.File(directory, key))
    }
    override suspend fun read(receiver: String): String? = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
        val stored = file(receiver)
        try {
            val raw = stored.openRead().use { input ->
                val bytes = ByteArray(513); var used = 0
                while (used < bytes.size) { val n = input.read(bytes, used, bytes.size - used); if (n < 0) break; used += n }
                require(used <= 512)
                String(bytes, 0, used, Charsets.UTF_8)
            }
            val json = org.json.JSONObject(raw)
            json.getString("token").takeIf { json.getString("receiver") == receiver && Regex("[a-f0-9]{64}").matches(it) }
        } catch (_: Exception) { null }
    }
    override suspend fun write(receiver: String, token: String?) = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
        val stored = file(receiver)
        if (token == null) stored.delete()
        else {
            require(Regex("[a-f0-9]{64}").matches(token))
            val stream = stored.startWrite()
            try {
                stream.write(org.json.JSONObject().put("receiver", receiver).put("token", token).toString().toByteArray())
                stored.finishWrite(stream)
                android.system.Os.chmod(stored.baseFile.path, 384) // 0600
            } catch (e: Exception) { stored.failWrite(stream); throw e }
        }
    }
}
