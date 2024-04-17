/*
------------------------------------------------------------------

This file is part of the Open Ephys GUI
Copyright (C) 2022 Open Ephys

------------------------------------------------------------------

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// TODO: some winsock porting is necessarey after all
// https://learn.microsoft.com/en-us/windows/win32/winsock/porting-socket-applications-to-winsock
#ifdef WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#endif

#include "UDPEventsPlugin.h"
#include "UDPEventsPluginEditor.h"

UDPEventsPlugin::UDPEventsPlugin()
    : GenericProcessor("UDP Events"), Thread("UDP Events Thread")
{
    // Host address to bind for receiving as a server.
    addStringParameter(Parameter::GLOBAL_SCOPE,
                       "host",
                       "Host address to bind for receiving UDP messages.",
                       "127.0.0.1",
                       true);

    // Host port to bind for receiving as a server.
    addIntParameter(Parameter::GLOBAL_SCOPE,
                    "port",
                    "Host port to bind for receiving UDP messages.",
                    12345,
                    0,
                    65535,
                    true);

    // Id of data stream to filter.
    addIntParameter(Parameter::GLOBAL_SCOPE,
                    "stream",
                    "Which data stream to filter",
                    0,
                    0,
                    65535,
                    true);

    // Real TTL line to use for sync events.
    StringArray syncLines;
    for (int i = 1; i <= 256; i++)
    {
        syncLines.add(String(i));
    }
    addCategoricalParameter(Parameter::GLOBAL_SCOPE,
                            "line",
                            "TTL line number where real sync events will occur",
                            syncLines,
                            0,
                            false);
}

UDPEventsPlugin::~UDPEventsPlugin()
{
}

AudioProcessorEditor *UDPEventsPlugin::createEditor()
{
    editor = std::make_unique<UDPEventsPluginEditor>(this);
    return editor.get();
}

void UDPEventsPlugin::parameterValueChanged(Parameter *param)
{
    if (param->getName().equalsIgnoreCase("host"))
    {
        hostToBind = param->getValueAsString();
    }
    else if (param->getName().equalsIgnoreCase("port"))
    {
        portToBind = (uint16)(int)param->getValue();
    }
    else if (param->getName().equalsIgnoreCase("stream"))
    {
        streamId = (uint16)(int)param->getValue();
    }
    else if (param->getName().equalsIgnoreCase("line"))
    {
        // The UI presents 1-based line numbers 1-256 but internal code uses 0-based 0-255.
        // Looks like getValue() for a categorical parameter gives the selection index.
        // Because of how we set up the categories above, selection index works as the 0-based line number.
        syncLine = (uint8)(int)param->getValue();
    }
}

bool UDPEventsPlugin::startAcquisition()
{
    /** Start with fresh sync estimates each acquisition.*/
    workingSync.clear();
    syncEstimates.clear();

    /** UDP socket and buffer lifecycle will match GUI acquisition periods. */
    startThread();
    return isThreadRunning();
}

bool UDPEventsPlugin::stopAcquisition()
{
    if (!stopThread(1000))
    {
        LOGE("UDP Events Thread timed out when trying ot stop.  Forcing termination, so things might be unstable going forward.");
        return false;
    }
    return true;
}

void UDPEventsPlugin::run()
{
    LOGD("UDP Events Thread is starting.");

    // Create a new UDP socket to receive on.
    int serverSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (serverSocket < 0)
    {
        LOGE("UDP Events Thread could not create a socket: ", serverSocket, " errno: ", errno, " -- ", strerror(errno));
        return;
    }

    // Bind the local address and port so we can receive, as a server.
    sockaddr_in addressToBind;
    addressToBind.sin_family = AF_INET;
    addressToBind.sin_port = htons(portToBind);
    addressToBind.sin_addr.s_addr = inet_addr(hostToBind.toUTF8());
    int bindResult = bind(serverSocket, (struct sockaddr *)&addressToBind, sizeof(addressToBind));
    if (bindResult < 0)
    {
        close(serverSocket);
        LOGE("UDP Events Thread could not bind socket to address: ", hostToBind, " port: ", portToBind, " errno: ", errno, " -- ", strerror(errno));
        return;
    }

    // Report the address and port we bound (it might have been assigned by system).
    sockaddr_in boundAddress;
    socklen_t boundNameLength = sizeof(boundAddress);
    getsockname(serverSocket, (struct sockaddr *)&boundAddress, &boundNameLength);
    uint16_t boundPort = ntohs(boundAddress.sin_port);
    char boundHost[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &boundAddress.sin_addr, boundHost, sizeof(boundHost));
    LOGD("UDP Events Thread is ready to receive at address: ", boundHost, " port: ", boundPort);

    // Configure to sleep until a message arrives, but wake every 100ms to be responsive to exit requests.
    struct pollfd toPoll[1];
    toPoll[0].fd = serverSocket;
    toPoll[0].events = POLLIN;
    int pollTimeoutMs = 100;

    // Read the client's address and message text into local buffers.
    sockaddr_in clientAddress;
    socklen_t clientAddressLength = sizeof(clientAddress);
    size_t messageBufferSize = 65536;
    char messageBuffer[messageBufferSize] = {0};
    while (!threadShouldExit())
    {
        // Sleep until a message arrives.
        int numReady = poll(toPoll, 1, pollTimeoutMs);
        if (numReady > 0 && toPoll[0].revents & POLLIN)
        {
            int bytesRead = recvfrom(serverSocket, messageBuffer, messageBufferSize - 1, 0, (struct sockaddr *)&clientAddress, &clientAddressLength);
            if (bytesRead <= 0)
            {
                LOGE("UDP Events Thread had a read error.  Bytes read: ", bytesRead, " errno: ", errno, " -- ", strerror(errno));
                continue;
            }

            // Record a timestamp close to when we got the UDP message.
            double serverSecs = (double)CoreServices::getSoftwareTimestamp() / CoreServices::getSoftwareSampleRate();

            // Who sent us this message?
            uint16_t clientPort = ntohs(clientAddress.sin_port);
            char clientHost[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddress.sin_addr, clientHost, sizeof(clientHost));
            LOGD("UDP Events Thread received ", bytesRead, " bytes from host: ", clientHost, " port: ", clientPort);

            // Acknowledge message receipt to the client.
            int bytesWritten = sendto(serverSocket, &serverSecs, 8, 0, reinterpret_cast<const sockaddr *>(&clientAddress), clientAddressLength);
            if (bytesWritten < 0)
            {
                LOGE("UDP Events Thread had a write error.  Bytes written: ", bytesWritten, " errno: ", errno, " -- ", strerror(errno));
                continue;
            }
            LOGD("UDP Events Thread sent ", bytesWritten, " bytes to host: ", clientHost, " port: ", clientPort);

            // Process the message itself.
            uint8 messageType = (uint8)messageBuffer[0];
            if (messageType == 1)
            {
                // This is a TTL message.
                SoftEvent ttlEvent;
                ttlEvent.type = 1;
                ttlEvent.clientSeconds = *((double *)(messageBuffer + 1));
                ttlEvent.lineNumber = (uint8)messageBuffer[9];
                ttlEvent.lineState = (uint8)messageBuffer[10];

                // Enqueue this to be handled below, on the main thread, in process().
                {
                    ScopedLock TTLlock(softEventQueueLock);
                    softEventQueue.push(ttlEvent);
                }
            }
            else if (messageType == 2)
            {
                // This is a Text message.
                SoftEvent textEvent;
                textEvent.type = 2;
                textEvent.clientSeconds = *((double *)(messageBuffer + 1));
                textEvent.textLength = ntohs(*((uint16 *)(messageBuffer + 9)));
                String text = String::fromUTF8(messageBuffer + 11, textEvent.textLength);
                textEvent.text = text + "@" + String(textEvent.clientSeconds);

                // Enqueue this to be handled below, on the main thread, in process().
                {
                    ScopedLock TTLlock(softEventQueueLock);
                    softEventQueue.push(textEvent);
                }
            }
            else
            {
                // This seems to be some unexpected message, and we'll ignore it.
                LOGE("UDP Events Thread ignoring message of unknown type ", (int)messageType, " and byte size ", bytesRead);
            }
        }
    }

    // The main loop has exited so we're done, so clean up and let the UDP thread terminate.
    close(serverSocket);
    LOGD("UDP Events Thread is stopping.");
}

EventChannel *UDPEventsPlugin::pickTTLChannel()
{
    for (auto eventChannel : eventChannels)
    {
        if (eventChannel->getType() == EventChannel::Type::TTL && eventChannel->getStreamId() == streamId)
        {
            return eventChannel;
        }
    }
    return nullptr;
}

void UDPEventsPlugin::process(AudioBuffer<float> &buffer)
{
    // This synchronously calls back to handleTTLEvent(), below.
    checkForEvents();


    // Find the selected data stream.
    for (auto stream : dataStreams)
    {
        if (stream->getStreamId() == streamId)
        {
            // Find a TTL channel for the selected data stream.
            EventChannel *ttlChannel = pickTTLChannel();
            if (ttlChannel == nullptr)
            {
                return;
            }

            // Work through soft messages enqueued above, by run() on the UDP Thread.
            {
                ScopedLock TTLlock(softEventQueueLock);
                while (!softEventQueue.empty())
                {
                    const SoftEvent &softEvent = softEventQueue.front();
                    softEventQueue.pop();
                    if (softEvent.type == 1)
                    {
                        // This is a TTL message.
                        if (softEvent.lineNumber == syncLine)
                        {
                            // This is a soft sync event corresponding to a real TTL event.
                            bool syncComplete = workingSync.recordSoftTimestamp(softEvent.clientSeconds, stream->getSampleRate());
                            if (syncComplete)
                            {
                                // The working sync has seen both a real and a soft event.
                                // Add it to the sync history and start a new sync going forward.
                                syncEstimates.push_back(workingSync);
                                workingSync.clear();
                            }
                        }
                        else
                        {
                            // This is a soft TTL event to add to the selected stream.
                            int64 sampleNumber = softSampleNumber(softEvent.clientSeconds, stream->getSampleRate());
                            if (sampleNumber)
                            {
                                // We'll only add it if we can find a previous sync estimate.
                                TTLEventPtr ttlEvent = TTLEvent::createTTLEvent(ttlChannel,
                                                                                sampleNumber,
                                                                                softEvent.lineNumber,
                                                                                softEvent.lineState);
                                addEvent(ttlEvent, 0);
                            }
                        }
                    }
                    else if (softEvent.type == 2)
                    {
                        // This is a Text message to add to the selected stream.
                        int64 sampleNumber = softSampleNumber(softEvent.clientSeconds, stream->getSampleRate());
                        if (sampleNumber)
                        {
                            // We'll only add it if we can find a previous sync estimate.
                            // For now, Open Ephys ignores the sampleNumber we pass for text events.
                            // But we can still do our best effort!
                            TextEventPtr textEvent = TextEvent::createTextEvent(getMessageChannel(),
                                                                                sampleNumber,
                                                                                softEvent.text);
                            addEvent(textEvent, 0);
                        }
                    }
                }
            }
        }
    }
}

int64 UDPEventsPlugin::softSampleNumber(double softSecs, float localSampleRate)
{
    // Look for the last completed sync estimate preceeding the given softSecs.
    for (auto current = syncEstimates.rbegin(); current != syncEstimates.rend(); ++current)
    {
        if (current->syncSoftSecs <= softSecs)
        {
            // This is the most relevant sync estimate.
            return current->softSampleNumber(softSecs, localSampleRate);
        }
    }

    // No relevant sync estimates.
    LOGE("UDP Events has no good sync estimate preceeding client soft secs: ", softSecs);
    return 0;
}

void UDPEventsPlugin::handleTTLEvent(TTLEventPtr event)
{
    if (event->getLine() == syncLine)
    {
        // This real TTL event should corredspond to a soft TTL event.
        for (auto stream : dataStreams)
        {
            if (stream->getStreamId() == streamId)
            {
                bool completed = workingSync.recordLocalSampleNumber(event->getSampleNumber(), stream->getSampleRate());
                if (completed)
                {
                    // The working sync has seen both a real and a soft event.
                    // Add it to the sync history and start a new sync going forward.
                    syncEstimates.push_back(workingSync);
                    workingSync.clear();
                }
            }
        }
    }
}
