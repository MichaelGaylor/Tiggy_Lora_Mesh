package com.loramesh.app.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Send
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.loramesh.app.data.ChatMessage
import com.loramesh.app.ui.MeshViewModel
import com.loramesh.app.ui.theme.*
import java.text.SimpleDateFormat
import java.util.*

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChatScreen(viewModel: MeshViewModel) {
    val messages by viewModel.messages.collectAsState()
    var input by remember { mutableStateOf("") }
    var targetId by remember { mutableStateOf("FFFF") }
    val listState = rememberLazyListState()

    // Auto-scroll to bottom on new messages
    LaunchedEffect(messages.size) {
        if (messages.isNotEmpty()) {
            listState.animateScrollToItem(messages.size - 1)
        }
    }

    Column(modifier = Modifier.fillMaxSize()) {
        // Target selector
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(MaterialTheme.colorScheme.surface)
                .padding(horizontal = 16.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("To: ", color = MeshGrey)
            OutlinedTextField(
                value = targetId,
                onValueChange = { if (it.length <= 4) targetId = it.uppercase() },
                modifier = Modifier.width(80.dp),
                singleLine = true,
                textStyle = MaterialTheme.typography.bodyMedium,
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = MeshCyan,
                    cursorColor = MeshCyan
                )
            )
            Spacer(Modifier.width(8.dp))
            Text(
                if (targetId == "FFFF") "Broadcast" else "Direct",
                color = if (targetId == "FFFF") MeshOrange else MeshGreen,
                style = MaterialTheme.typography.bodySmall
            )
        }

        // Messages
        LazyColumn(
            modifier = Modifier
                .weight(1f)
                .padding(horizontal = 12.dp),
            state = listState,
            verticalArrangement = Arrangement.spacedBy(6.dp),
            contentPadding = PaddingValues(vertical = 8.dp)
        ) {
            items(messages) { msg ->
                ChatBubble(msg)
            }
        }

        // Input bar
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(MaterialTheme.colorScheme.surface)
                .padding(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            OutlinedTextField(
                value = input,
                onValueChange = { input = it },
                modifier = Modifier.weight(1f),
                placeholder = { Text("Type a message...") },
                singleLine = true,
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = MeshCyan,
                    cursorColor = MeshCyan
                )
            )
            Spacer(Modifier.width(8.dp))
            IconButton(
                onClick = {
                    if (input.isNotBlank()) {
                        if (targetId == "FFFF") {
                            viewModel.sendBroadcast(input.trim())
                        } else {
                            viewModel.sendMessage(targetId, input.trim())
                        }
                        input = ""
                    }
                }
            ) {
                Icon(Icons.Default.Send, contentDescription = "Send", tint = MeshCyan)
            }
        }
    }
}

@Composable
private fun ChatBubble(msg: ChatMessage) {
    val isOut = msg.isOutgoing
    val bubbleColor = if (isOut) MeshGreen.copy(alpha = 0.15f) else MeshBlue.copy(alpha = 0.15f)
    val borderColor = if (isOut) MeshGreen else MeshBlue
    val timeFormat = SimpleDateFormat("HH:mm", Locale.getDefault())

    Column(
        modifier = Modifier.fillMaxWidth(),
        horizontalAlignment = if (isOut) Alignment.End else Alignment.Start
    ) {
        // Sender label
        Text(
            text = if (isOut) "You" else msg.from,
            style = MaterialTheme.typography.labelSmall,
            color = borderColor,
            modifier = Modifier.padding(horizontal = 8.dp)
        )

        Box(
            modifier = Modifier
                .widthIn(max = 280.dp)
                .clip(
                    RoundedCornerShape(
                        topStart = 12.dp, topEnd = 12.dp,
                        bottomStart = if (isOut) 12.dp else 2.dp,
                        bottomEnd = if (isOut) 2.dp else 12.dp
                    )
                )
                .background(bubbleColor)
                .padding(horizontal = 12.dp, vertical = 8.dp)
        ) {
            Column {
                Text(msg.text, color = Color.White)
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End
                ) {
                    if (!isOut && msg.rssi != 0) {
                        Text(
                            "${msg.rssi}dBm",
                            style = MaterialTheme.typography.labelSmall,
                            color = MeshGrey
                        )
                        Spacer(Modifier.width(8.dp))
                    }
                    Text(
                        timeFormat.format(Date(msg.timestamp)),
                        style = MaterialTheme.typography.labelSmall,
                        color = MeshGrey
                    )
                }
            }
        }
    }
}
