package com.hr54.controller

import androidx.test.core.app.ApplicationProvider
import com.hr54.controller.data.repository.PrivateManagementTokens
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import android.content.Context
import java.io.File

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [36])
class TokenStorageTest {
    @Test fun managementTokensArePrivateBackupExcludedAndScopedToReceiver() = runBlocking {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val tokens = PrivateManagementTokens(context)
        val first = "http://first:8130"; val second = "http://second:8130"; val token = "a".repeat(64)
        tokens.write(first, token); assertEquals(token, PrivateManagementTokens(context).read(first)); assertNull(tokens.read(second))
        val directory = File(context.noBackupFilesDir, "module-tokens")
        assertTrue(directory.isDirectory); assertTrue(directory.listFiles()!!.all { Regex("[a-f0-9]{64}").matches(it.name) })
        tokens.write(second, "b".repeat(64)); tokens.write(first, null)
        assertNull(tokens.read(first)); assertEquals("b".repeat(64), tokens.read(second))
        tokens.write(second, null)
    }
}
