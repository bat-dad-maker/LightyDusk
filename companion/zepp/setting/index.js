AppSettingsPage({
  build(props) {
    return Section({}, [
      Text({ style: { fontSize: '20px', fontWeight: 'bold' } }, ['BOTWRecompAndroid • Watch map']),
      Text({}, ['On Android, long-press the BOTWRecompAndroid icon and open “Watch map”. Turn on sharing and copy the code below.']),
      TextInput({
        label: 'Connection code',
        placeholder: 'Paste the 32-character code',
        value: props.settingsStorage.getItem('connectionToken') || '',
        onChange: (value) => props.settingsStorage.setItem('connectionToken', value.trim().toLowerCase())
      }),
      Text({}, ['Open the game and Recomp Map on the watch. Turn the crown to zoom, drag to explore, and tap Center Link to follow the character again.']),
      Text({}, ['The map is stored on the watch. The paired phone only sends position, heading and markers through Zepp, with no internet server.'])
    ])
  }
})
