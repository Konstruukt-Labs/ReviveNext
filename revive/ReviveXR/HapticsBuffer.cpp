#include "HapticsBuffer.h"

HapticsBuffer::HapticsBuffer()
	: m_ReadIndex(0xFF)
	, m_WriteIndex(0)
	, m_Buffer()
{
}

void HapticsBuffer::AddSamples(const ovrHapticsBuffer* buffer)
{
	uint8_t* samples = (uint8_t*)buffer->Samples;

	for (int i = 0; i < buffer->SamplesCount; i++)
	{
		if (m_WriteIndex == m_ReadIndex)
			return;

		m_Buffer[m_WriteIndex] = samples[i];

		// We increment the atomic here as a memory barrier
		// Index will overflow correctly, so no need for a modulo operator
		m_WriteIndex++;
	}
}

float HapticsBuffer::GetSample()
{
	// We can't pass the write index, so the buffer is now empty
	if (m_ReadIndex + 1 == m_WriteIndex)
		return 0.0f;

	// Index will overflow correctly, so no need for a modulo operator
	uint8_t sample = m_Buffer[++m_ReadIndex];

	return sample / 255.0f;
}

ovrHapticsPlaybackState HapticsBuffer::GetState()
{
	ovrHapticsPlaybackState state = { 0 };

	// Same result as counting up from WriteIndex to ReadIndex (and from
	// ReadIndex to WriteIndex) one increment at a time, up to 255
	// iterations each - the loop's iteration count is exactly the 8-bit
	// wraparound difference between the two indices, so computing that
	// directly gets the same answer without the loop. RemainingQueueSpace/
	// SamplesQueued are plain int fields (not 8-bit), so the +1/-1 below
	// must NOT wrap the way the uint8_t subtraction inside them does.
	uint8_t writeToRead = (uint8_t)(m_ReadIndex - m_WriteIndex);
	uint8_t readToWrite = (uint8_t)(m_WriteIndex - m_ReadIndex);
	state.RemainingQueueSpace = (int)writeToRead + 1;
	state.SamplesQueued = (int)readToWrite - 1;

	return state;
}
