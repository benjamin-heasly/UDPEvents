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

#ifndef UDPEVENTSPLUGINEDITOR_H_DEFINED
#define UDPEVENTSPLUGINEDITOR_H_DEFINED

#include <EditorHeaders.h>

class UDPEventsPluginEditor : public GenericEditor,
							  public ComboBox::Listener
{
public:
	/** Constructor */
	UDPEventsPluginEditor(GenericProcessor *parentNode);

	/** Destructor */
	~UDPEventsPluginEditor() {}

	/** Called when underlying settings are updated */
	void updateSettings() override;

	/** Called when a ComboBox changes*/
	void comboBoxChanged(ComboBox* comboBox) override;

	/** Called just prior to the start of acquisition, to allow custom commands. */
	void startAcquisition() override;

	/** Called after the end of acquisition, to allow custom commands .*/
	void stopAcquisition() override;

private:
	/** Dynamic combo box for selecting a stream by name.*/
	std::unique_ptr<ComboBox> streamSelection;

	/** Generates an assertion if this class leaks */
	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UDPEventsPluginEditor);
};

#endif // UDPEventsPluginEDITOR_H_DEFINED